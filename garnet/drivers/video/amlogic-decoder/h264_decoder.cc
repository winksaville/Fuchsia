// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "h264_decoder.h"

#include <fbl/algorithm.h>
#include <lib/media/codec_impl/codec_buffer.h>
#include <lib/media/codec_impl/codec_frame.h>
#include <lib/media/codec_impl/codec_packet.h>
#include <lib/zx/vmo.h>

#include "firmware_blob.h"
#include "macros.h"
#include "memory_barriers.h"
#include "pts_manager.h"
#include "util.h"

// TODO(35200):
//
// Change these to InternalBuffer:
//
// InputContext::buffer - optionally secure
// (done) reference_mv_buffer_ - optionally secure
// (done) codec_data_ - optionally secure
// (done) sei_data_buffer_ - optionally secure
//
// Plumb is_secure to each of the above.
//
// (Fine as io_bufer_t for now:
//    * loading firmware can use clear memory, since those are just reads by the HW, and we can only
//      load firmware if we can write the firmware bits to RAM using REE CPU.
//    * secondary_firmware_ - never secure)

static const uint32_t kBufferAlignShift = 4 + 12;
static const uint32_t kBufferAlign = 1 << kBufferAlignShift;

// AvScratch1
class StreamInfo : public TypedRegisterBase<DosRegisterIo, StreamInfo, uint32_t> {
 public:
  DEF_FIELD(7, 0, width_in_mbs);
  DEF_FIELD(23, 8, total_mbs);
  DEF_FIELD(30, 24, max_reference_size);
  DEF_BIT(31, mv_size_flag);

  static auto Get() { return AddrType(0x09c1 * 4); }
};

// AvScratch2
class SequenceInfo : public TypedRegisterBase<DosRegisterIo, SequenceInfo, uint32_t> {
 public:
  DEF_BIT(0, aspect_ratio_info_present_flag);
  DEF_BIT(1, timing_info_present_flag);
  DEF_BIT(4, pic_struct_present_flag);

  // relatively lower-confidence vs. other bits - not confirmed
  DEF_BIT(6, fixed_frame_rate_flag);

  DEF_FIELD(14, 13, chroma_format_idc);
  DEF_BIT(15, frame_mbs_only_flag);
  DEF_FIELD(23, 16, aspect_ratio_idc);

  static auto Get() { return AddrType(0x09c2 * 4); }
};

// AvScratch3
class SampleAspectRatioInfo
    : public TypedRegisterBase<DosRegisterIo, SampleAspectRatioInfo, uint32_t> {
 public:
  DEF_FIELD(15, 0, sar_width);
  DEF_FIELD(31, 16, sar_height);

  static auto Get() { return AddrType(0x09c3 * 4); }
};

// AvScratch6
class CropInfo : public TypedRegisterBase<DosRegisterIo, CropInfo, uint32_t> {
 public:
  // All quantities are the number of pixels to be cropped from each side.
  DEF_FIELD(7, 0, bottom);
  DEF_FIELD(15, 8, top);  // Ignored
  DEF_FIELD(23, 16, right);
  DEF_FIELD(31, 24, left);  // Ignored

  static auto Get() { return AddrType(0x09c6 * 4); }
};

// AvScratchF
class CodecSettings : public TypedRegisterBase<DosRegisterIo, CodecSettings, uint32_t> {
 public:
  DEF_BIT(1, trickmode_i);
  DEF_BIT(2, zeroed0);
  DEF_BIT(3, drop_b_frames);
  DEF_BIT(4, error_recovery_mode);
  DEF_BIT(5, zeroed1);
  DEF_BIT(6, ip_frames_only);
  DEF_BIT(7, disable_fast_poc);

  static auto Get() { return AddrType(0x09cf * 4); }
};

// AvScratchInfo1+
class PicInfo : public TypedRegisterBase<DosRegisterIo, PicInfo, uint32_t> {
 public:
  DEF_FIELD(4, 0, buffer_index);
  DEF_BIT(9, error);
  DEF_BIT(15, eos);
  DEF_FIELD(31, 16, stream_offset);

  static auto Get(uint32_t i) { return AddrType((0x09c1 + i) * 4); }
};

// 0 means "Unspecified"
constexpr uint32_t kAspectRatioIdcExtendedSar = 255;

// This struct type doesn't need a name, since we only read this one static
// instance.
struct {
  const uint8_t sar_width;
  const uint8_t sar_height;
} kSarTable[] = {
    // 0 - entry 0 in this table is never read, but it's only 2 bytes so we just
    // let it exist since subtracting 1 from aspect_ratio_idc would probably
    // take
    // ~2 code bytes or more anyway.
    {0, 0},
    // 1
    {1, 1},
    // 2
    {12, 11},
    // 3
    {10, 11},
    // 4
    {16, 11},
    // 5
    {40, 33},
    // 6
    {24, 11},
    // 7
    {20, 11},
    // 8
    {32, 11},
    // 9
    {80, 33},
    // 10
    {18, 11},
    // 11
    {15, 11},
    // 12
    {64, 33},
    // 13
    {160, 99},
    // 14
    {4, 3},
    // 15
    {3, 2},
    // 16
    {2, 1},
};

static uint32_t GetMaxDpbSize(uint32_t level_idc, uint32_t width_in_mbs, uint32_t height_in_mbs) {
  // From Table A-1 of the h.264 spec.
  // https://www.itu.int/rec/T-REC-H.264-201704-I/en
  uint32_t max_dpb_mbs;
  switch (level_idc) {
    case 10:
      max_dpb_mbs = 396;
      break;
    case 11:
      max_dpb_mbs = 900;
      break;
    case 12:
    case 13:
    case 20:
      max_dpb_mbs = 2376;
      break;
    case 21:
      max_dpb_mbs = 4752;
      break;
    case 22:
    case 30:
      max_dpb_mbs = 8100;
      break;
    case 31:
      max_dpb_mbs = 18000;
      break;
    case 32:
      max_dpb_mbs = 20480;
      break;
    case 40:
    case 41:
      max_dpb_mbs = 32768;
      break;
    case 42:
      max_dpb_mbs = 34816;
      break;
    case 50:
      max_dpb_mbs = 110400;
      break;
    case 51:
    case 52:
      max_dpb_mbs = 184320;
      break;
    case 60:
    case 61:
    case 62:
      max_dpb_mbs = 696320;
      break;
    default:
      return 0;
  }

  uint32_t num_mbs = width_in_mbs * height_in_mbs;
  if (!num_mbs)
    return 0;
  return std::min(16u, (max_dpb_mbs + num_mbs - 1) / num_mbs);
}

H264Decoder::~H264Decoder() {
  owner_->core()->StopDecoding();
  owner_->core()->WaitForIdle();
  BarrierBeforeRelease();
  io_buffer_release(&secondary_firmware_);
  // ~reference_mv_buffer_
  // ~sei_data_buffer_
  // ~codec_data_
}

zx_status_t H264Decoder::ResetHardware() {
  DosSwReset0::Get().FromValue((1 << 7) | (1 << 6) | (1 << 4)).WriteTo(owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Reads are used for delaying running later code.
  for (uint32_t i = 0; i < 3; i++) {
    DosSwReset0::Get().ReadFrom(owner_->dosbus());
  }

  DosSwReset0::Get().FromValue((1 << 7) | (1 << 6) | (1 << 4)).WriteTo(owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  DosSwReset0::Get().FromValue((1 << 9) | (1 << 8)).WriteTo(owner_->dosbus());
  DosSwReset0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Reads are used for delaying running later code.
  for (uint32_t i = 0; i < 3; i++) {
    DosSwReset0::Get().ReadFrom(owner_->dosbus());
  }

  auto temp = PowerCtlVld::Get().ReadFrom(owner_->dosbus());
  temp.set_reg_value(temp.reg_value() | (1 << 9) | (1 << 6));
  temp.WriteTo(owner_->dosbus());

  return ZX_OK;
}

zx_status_t H264Decoder::LoadSecondaryFirmware(const uint8_t* data, uint32_t firmware_size) {
  // For some reason, some portions of the firmware aren't loaded into the
  // hardware directly, but are kept in main memory.
  constexpr uint32_t kSecondaryFirmwareSize = 4 * 1024;
  constexpr uint32_t kSecondaryFirmwareBufferSize = kSecondaryFirmwareSize * 5;
  {
    zx_status_t status =
        io_buffer_init_aligned(&secondary_firmware_, owner_->bti()->get(),
                               kSecondaryFirmwareBufferSize, kBufferAlignShift,
                               IO_BUFFER_RW | IO_BUFFER_CONTIG);
    if (status != ZX_OK) {
      DECODE_ERROR("Failed to make second firmware buffer: %d", status);
      return status;
    }
    SetIoBufferName(&secondary_firmware_, "H264SecondaryFirmware");

    auto addr = static_cast<uint8_t*>(io_buffer_virt(&secondary_firmware_));
    // The secondary firmware is in a different order in the file than the main
    // firmware expects it to have.
    memcpy(addr + 0, data + 0x4000, kSecondaryFirmwareSize);       // header
    memcpy(addr + 0x1000, data + 0x2000, kSecondaryFirmwareSize);  // data
    memcpy(addr + 0x2000, data + 0x6000, kSecondaryFirmwareSize);  // mmc
    memcpy(addr + 0x3000, data + 0x3000, kSecondaryFirmwareSize);  // list
    memcpy(addr + 0x4000, data + 0x5000, kSecondaryFirmwareSize);  // slice
  }
  io_buffer_cache_flush(&secondary_firmware_, 0, kSecondaryFirmwareBufferSize);
  return ZX_OK;
}

zx_status_t H264Decoder::Initialize() {
  uint8_t* data;
  uint32_t firmware_size;
  zx_status_t status =
      owner_->SetProtected(VideoDecoder::Owner::ProtectableHardwareUnit::kVdec, false);
  if (status != ZX_OK)
    return status;

  status = owner_->firmware_blob()->GetFirmwareData(FirmwareBlob::FirmwareType::kH264, &data,
                                                    &firmware_size);
  if (status != ZX_OK)
    return status;
  status = owner_->core()->LoadFirmware(data, firmware_size);
  if (status != ZX_OK)
    return status;

  if (!WaitForRegister(std::chrono::milliseconds(100), [this]() {
        return !(DcacDmaCtrl::Get().ReadFrom(owner_->dosbus()).reg_value() & 0x8000);
      })) {
    DECODE_ERROR("Waiting for DCAC DMA timed out\n");
    return ZX_ERR_TIMED_OUT;
  }

  if (!WaitForRegister(std::chrono::milliseconds(100), [this]() {
        return !(LmemDmaCtrl::Get().ReadFrom(owner_->dosbus()).reg_value() & 0x8000);
      })) {
    DECODE_ERROR("Waiting for LMEM DMA timed out\n");
    return ZX_ERR_TIMED_OUT;
  }

  status = ResetHardware();
  if (status != ZX_OK)
    return status;

  PscaleCtrl::Get().FromValue(0).WriteTo(owner_->dosbus());
  AvScratch0::Get().FromValue(0).WriteTo(owner_->dosbus());

  // TODO(34192): After sysmem has min_base_phys_address_divisor, use that to avoid over-allocating
  // and rounding up here.
  const uint32_t kCodecDataSize = 0x1ee000 + kBufferAlign;
  auto codec_data_create_result = InternalBuffer::Create(
      "H264CodecData", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(), kCodecDataSize, is_secure_,
      /*is_writable=*/true, /*is_mapping_needed*/false);
  if (!codec_data_create_result.is_ok()) {
    LOG(ERROR, "Failed to make codec data buffer - status: %d", codec_data_create_result.error());
    return codec_data_create_result.error();
  }
  codec_data_.emplace(codec_data_create_result.take_value());
  zx_paddr_t aligned_codec_data_phys = fbl::round_up(codec_data_->phys_base(), kBufferAlign);
  // sysmem ensures that newly allocated buffers are zeroed and flushed, to extent possible, so
  // codec_data_ doesn't need CacheFlush() here.

  status = LoadSecondaryFirmware(data, firmware_size);
  if (status != ZX_OK)
    return status;

  enum {
    kBufferStartAddressOffset = 0x1000000,
  };

  BarrierAfterFlush();  // For codec_data and secondary_firmware_

  // This may wrap if the address is less than the buffer start offset.
  uint32_t buffer_offset = truncate_to_32(aligned_codec_data_phys) - kBufferStartAddressOffset;
  AvScratch1::Get().FromValue(buffer_offset).WriteTo(owner_->dosbus());
  AvScratchG::Get()
      .FromValue(truncate_to_32(io_buffer_phys(&secondary_firmware_)))
      .WriteTo(owner_->dosbus());
  AvScratch7::Get().FromValue(0).WriteTo(owner_->dosbus());
  AvScratch8::Get().FromValue(0).WriteTo(owner_->dosbus());
  AvScratch9::Get().FromValue(0).WriteTo(owner_->dosbus());
  VdecAssistMbox1ClrReg::Get().FromValue(1).WriteTo(owner_->dosbus());
  VdecAssistMbox1Mask::Get().FromValue(1).WriteTo(owner_->dosbus());
  MdecPicDcCtrl::Get().ReadFrom(owner_->dosbus()).set_nv12_output(true).WriteTo(owner_->dosbus());
  CodecSettings::Get()
      .ReadFrom(owner_->dosbus())
      .set_zeroed0(0)
      .set_drop_b_frames(false)
      .set_error_recovery_mode(1)
      .set_zeroed1(0)
      .set_ip_frames_only(0)
      .set_disable_fast_poc(0)
      .WriteTo(owner_->dosbus());

  // TODO(34192): After sysmem has min_base_phys_address_divisor, use that to avoid over-allocating
  // and rounding up here.
  constexpr uint32_t kSeiBufferSize = 8 * 1024 + kBufferAlign;
  auto sei_create_result = InternalBuffer::Create(
      "H264SeiData", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(), kSeiBufferSize, is_secure_,
      /*is_writable=*/true, /*is_mapping_neede=*/false);
  if (!sei_create_result.is_ok()) {
    LOG(ERROR, "Failed to make sei data buffer - status: %d", sei_create_result.error());
    return sei_create_result.error();
  }
  sei_data_buffer_.emplace(sei_create_result.take_value());
  zx_paddr_t sei_data_buffer_aligned_phys = fbl::round_up(sei_data_buffer_->phys_base(), kBufferAlign);
  // Sysmem has zeroed sei_data_buffer_, flushed the zeroes, and fenced the flush, to extent
  // possible.

  AvScratchI::Get()
      .FromValue(truncate_to_32(sei_data_buffer_aligned_phys) - buffer_offset)
      .WriteTo(owner_->dosbus());
  AvScratchJ::Get().FromValue(0).WriteTo(owner_->dosbus());
  MdecPicDcThresh::Get().FromValue(0x404038aa).WriteTo(owner_->dosbus());

  owner_->core()->StartDecoding();
  return ZX_OK;
}

void H264Decoder::SetFrameReadyNotifier(FrameReadyNotifier notifier) {
  notifier_ = std::move(notifier);
}

void H264Decoder::SetInitializeFramesHandler(InitializeFramesHandler handler) {
  initialize_frames_handler_ = std::move(handler);
}

void H264Decoder::SetErrorHandler(fit::closure error_handler) {
  error_handler_ = std::move(error_handler);
}

void H264Decoder::InitializedFrames(std::vector<CodecFrame> frames, uint32_t coded_width,
                                    uint32_t coded_height, uint32_t stride) {
  ZX_DEBUG_ASSERT(state_ == DecoderState::kWaitingForNewFrames);
  ZX_DEBUG_ASSERT(coded_width == stride);
  uint32_t frame_count = frames.size();
  for (uint32_t i = 0; i < frame_count; ++i) {
    auto frame = std::make_shared<VideoFrame>();
    // While we'd like to pass in IO_BUFFER_CONTIG, since we know the VMO was
    // allocated with zx_vmo_create_contiguous(), the io_buffer_init_vmo()
    // treats that flag as an invalid argument, so instead we have to pretend as
    // if it's a non-contiguous VMO, then validate that the VMO is actually
    // contiguous later in aml_canvas_config() called by
    // owner_->ConfigureCanvas() below.
    assert(frames[i].codec_buffer_spec.has_data());
    assert(frames[i].codec_buffer_spec.data().is_vmo());
    assert(frames[i].codec_buffer_spec.data().vmo().has_vmo_handle());
    zx_status_t status = io_buffer_init_vmo(
        &frame->buffer, owner_->bti()->get(),
        frames[i].codec_buffer_spec.data().vmo().vmo_handle().get(), 0, IO_BUFFER_RW);
    if (status != ZX_OK) {
      DECODE_ERROR("Failed to io_buffer_init_vmo() for frame - status: %d\n", status);
      OnFatalError();
      return;
    }
    io_buffer_cache_flush(&frame->buffer, 0, io_buffer_size(&frame->buffer, 0));

    BarrierAfterFlush();

    frame->hw_width = coded_width;
    frame->hw_height = coded_height;
    frame->coded_width = coded_width;
    frame->coded_height = coded_height;
    frame->stride = stride;
    frame->uv_plane_offset = stride * coded_height;
    frame->display_width = display_width_;
    frame->display_height = display_height_;
    frame->index = i;

    // can be nullptr
    frame->codec_buffer = frames[i].codec_buffer_ptr;
    if (frames[i].codec_buffer_ptr) {
      frames[i].codec_buffer_ptr->SetVideoFrame(frame);
    }

    // The ConfigureCanvas() calls validate that the VMO is physically
    // contiguous, regardless of how the VMO was created.
    auto y_canvas =
        owner_->ConfigureCanvas(&frame->buffer, 0, frame->stride, frame->coded_height, 0, 0);
    auto uv_canvas = owner_->ConfigureCanvas(&frame->buffer, frame->uv_plane_offset, frame->stride,
                                             frame->coded_height / 2, 0, 0);
    if (!y_canvas || !uv_canvas) {
      OnFatalError();
      return;
    }

    AncNCanvasAddr::Get(i)
        .FromValue((uv_canvas->index() << 16) | (uv_canvas->index() << 8) | (y_canvas->index()))
        .WriteTo(owner_->dosbus());
    video_frames_.push_back({std::move(frame), std::move(y_canvas), std::move(uv_canvas)});
  }
  AvScratch0::Get().FromValue(next_av_scratch0_).WriteTo(owner_->dosbus());
  state_ = DecoderState::kRunning;
}

zx_status_t H264Decoder::InitializeFrames(uint32_t frame_count, uint32_t coded_width,
                                          uint32_t coded_height, uint32_t display_width,
                                          uint32_t display_height, bool has_sar, uint32_t sar_width,
                                          uint32_t sar_height) {
  DLOG("InitializeFrames() display_width: %u display_height: %u", display_width, display_height);
  video_frames_.clear();
  returned_frames_.clear();

  uint32_t stride = coded_width;
  uint64_t frame_vmo_bytes = stride * coded_height * 3 / 2;
  display_width_ = display_width;
  display_height_ = display_height;

  // Regardless of local allocation of VMOs or remote allocation of VMOs, we
  // first represent the frames this way.  This representation conveys the
  // potentially-non-zero offset into the VMO, and allows sharing code further
  // down.
  std::vector<CodecFrame> frames;
  if (initialize_frames_handler_) {
    ::zx::bti duplicated_bti;
    zx_status_t dup_result = owner_->bti()->duplicate(ZX_RIGHT_SAME_RIGHTS, &duplicated_bti);
    if (dup_result != ZX_OK) {
      DECODE_ERROR("Failed to duplicate BTI - status: %d\n", dup_result);
      return dup_result;
    }
    zx_status_t initialize_result = initialize_frames_handler_(
        std::move(duplicated_bti), frame_count, coded_width, coded_height, stride, display_width,
        display_height, has_sar, sar_width, sar_height);
    if (initialize_result != ZX_OK) {
      if (initialize_result != ZX_ERR_STOP) {
        DECODE_ERROR("initialize_frames_handler_() failed - status: %d\n", initialize_result);
      }
      return initialize_result;
    }
  } else {
    for (uint32_t i = 0; i < frame_count; ++i) {
      // aml_canvas_config() requires contiguous VMOs, and will validate that
      // each frame VMO is actually physically contiguous.  So create with
      // zx_vmo_create_contiguous() here.
      ::zx::vmo frame_vmo;
      zx_status_t vmo_create_result = zx_vmo_create_contiguous(
          owner_->bti()->get(), frame_vmo_bytes, 0, frame_vmo.reset_and_get_address());
      if (vmo_create_result != ZX_OK) {
        DECODE_ERROR("H264Decoder::InitializeFrames() failed - status: %d\n", vmo_create_result);
        return vmo_create_result;
      }
      fuchsia::media::StreamBufferData codec_buffer_data;
      fuchsia::media::StreamBufferDataVmo data_vmo;
      data_vmo.set_vmo_handle(std::move(frame_vmo));
      data_vmo.set_vmo_usable_start(0);
      data_vmo.set_vmo_usable_size(frame_vmo_bytes);
      codec_buffer_data.set_vmo(std::move(data_vmo));
      fuchsia::media::StreamBuffer buffer;
      buffer.set_buffer_lifetime_ordinal(next_non_codec_buffer_lifetime_ordinal_);
      buffer.set_buffer_index(i);
      buffer.set_data(std::move(codec_buffer_data));
      frames.emplace_back(CodecFrame{
          .codec_buffer_spec = std::move(buffer),
          .codec_buffer_ptr = nullptr,
      });
    }
    next_non_codec_buffer_lifetime_ordinal_++;
    InitializedFrames(std::move(frames), coded_width, coded_height, stride);
  }

  return ZX_OK;
}

void H264Decoder::ReturnFrame(std::shared_ptr<VideoFrame> video_frame) {
  returned_frames_.push_back(video_frame);
  TryReturnFrames();
}

void H264Decoder::TryReturnFrames() {
  while (!returned_frames_.empty()) {
    std::shared_ptr<VideoFrame> frame = returned_frames_.back();
    if (frame->index >= video_frames_.size() || frame != video_frames_[frame->index].frame) {
      // Possible if the stream size changed.
      returned_frames_.pop_back();
      continue;
    }
    if (AvScratch7::Get().ReadFrom(owner_->dosbus()).reg_value() == 0) {
      AvScratch7::Get().FromValue(frame->index + 1).WriteTo(owner_->dosbus());
    } else if (AvScratch8::Get().ReadFrom(owner_->dosbus()).reg_value() == 0) {
      AvScratch8::Get().FromValue(frame->index + 1).WriteTo(owner_->dosbus());
    } else {
      // Neither return slot is free, so give up for now. An interrupt
      // signaling completion of a frame should cause this to be tried again.
      // TODO: Try returning frames again after a delay, to ensure this won't
      // hang forever.
      return;
    }
    returned_frames_.pop_back();
  }
}

zx_status_t H264Decoder::InitializeStream() {
  ZX_DEBUG_ASSERT(state_ == DecoderState::kRunning);
  state_ = DecoderState::kWaitingForNewFrames;
  BarrierBeforeRelease();  // For reference_mv_buffer_
  // Ensure empty; may or may not be set at this point.
  reference_mv_buffer_.reset();
  // StreamInfo AKA AvScratch1.
  auto stream_info = StreamInfo::Get().ReadFrom(owner_->dosbus());
  // SequenceInfo AKA AvScratch2.
  auto sequence_info = SequenceInfo::Get().ReadFrom(owner_->dosbus());
  // SampleAspectRatioInfo AKA AvScratch3
  auto sar_info = SampleAspectRatioInfo::Get().ReadFrom(owner_->dosbus());
  uint32_t level_idc = AvScratchA::Get().ReadFrom(owner_->dosbus()).reg_value();
  uint32_t mb_mv_byte = stream_info.mv_size_flag() ? 24 : 96;
  uint32_t mb_width = stream_info.width_in_mbs();
  if (!mb_width && stream_info.total_mbs())
    mb_width = 256;
  if (!mb_width) {
    DECODE_ERROR("Width is 0 macroblocks\n");
    // Not returning ZX_ERR_IO_DATA_INTEGRITY, because this isn't an explicit
    // integrity check.
    return ZX_ERR_INTERNAL;
  }
  uint32_t mb_height = stream_info.total_mbs() / mb_width;

  constexpr uint32_t kActualDPBSize = 24;
  uint32_t max_dpb_size = GetMaxDpbSize(level_idc, mb_width, mb_height);
  if (max_dpb_size == 0) {
    max_dpb_size = kActualDPBSize;
  } else {
    max_dpb_size = std::min(max_dpb_size, kActualDPBSize);
  }
  uint32_t max_reference_size = std::min(stream_info.max_reference_size(), kActualDPBSize - 1);
  max_dpb_size = std::max(max_reference_size, max_dpb_size);
  max_reference_size++;

  // Rounding to 4 macroblocks is for matching the linux driver, in case the
  // hardware happens to round up as well.
  uint32_t mv_buffer_size =
      fbl::round_up(mb_height, 4u) * fbl::round_up(mb_width, 4u) * mb_mv_byte * max_reference_size;
  uint32_t mv_buffer_alloc_size = fbl::round_up(mv_buffer_size, ZX_PAGE_SIZE);

  auto create_result = InternalBuffer::Create(
      "H264ReferenceMvs", &owner_->SysmemAllocatorSyncPtr(), owner_->bti(), mv_buffer_alloc_size,
      is_secure_, /*is_writable=*/true, /*is_mapping_needed*/false);
  if (!create_result.is_ok()) {
    LOG(ERROR, "Couldn't allocate reference mv buffer - status: %d", create_result.error());
    return create_result.error();
  }
  reference_mv_buffer_.emplace(create_result.take_value());

  // sysmem ensure that newly allocated buffers are zeroed and flushed to RAM and fenced, to the
  // degree possible. 

  BarrierAfterFlush();
  AvScratch1::Get()
      .FromValue(truncate_to_32(reference_mv_buffer_->phys_base()))
      .WriteTo(owner_->dosbus());
  // In the linux driver AvScratch3 is used to communicate about the display
  // canvas.
  AvScratch3::Get().FromValue(0).WriteTo(owner_->dosbus());
  AvScratch4::Get()
      .FromValue(truncate_to_32(reference_mv_buffer_->phys_base() + mv_buffer_size))
      .WriteTo(owner_->dosbus());

  auto crop_info = CropInfo::Get().ReadFrom(owner_->dosbus());
  uint32_t display_width = mb_width * 16 - crop_info.right();
  uint32_t display_height = mb_height * 16 - crop_info.bottom();

  // Canvas width must be a multiple of 32 bytes.
  uint32_t coded_width = fbl::round_up(mb_width * 16, 32u);
  uint32_t coded_height = mb_height * 16;

  // Sample aspect ratio - normalize as sar_width : sar_height.
  //
  // The has_sar will be true for any explicitly-specified SAR, and false for
  // all other cases (both explicitly "Unspecified" and "Reserved" cases that we
  // don't recognize).
  bool has_sar = false;
  uint32_t sar_width = 1;
  uint32_t sar_height = 1;
  if (sequence_info.aspect_ratio_info_present_flag()) {
    uint32_t aspect_ratio_idc = sequence_info.aspect_ratio_idc();
    if (aspect_ratio_idc == kAspectRatioIdcExtendedSar) {
      sar_width = sar_info.sar_width();
      sar_height = sar_info.sar_height();
      has_sar = true;
      if (sar_width == 0 || sar_height == 0) {
        // spec says this condition means "considered unspecified"
        sar_width = 1;
        sar_height = 1;
        has_sar = false;
      }
    } else {
      ZX_DEBUG_ASSERT(aspect_ratio_idc != kAspectRatioIdcExtendedSar);
      // aspect_ratio_idc == 0 and "Reserved" values are treated the same way as
      // each other, and both cases don't run the body of the following "if". We
      // treat "Reserved" the same as "Unspecified" instead of flagging an error
      // because it seems extremely unlikely that any "Reserved" value in this
      // context would have meaning beyond specifying sar_width and sar_height.
      // So for "Reserved" values we just end up with has_sar false, which
      // should allow _something_ to be displayed even if the displayed frames
      // have the wrong SAR.
      if (aspect_ratio_idc >= 1 && aspect_ratio_idc <= 16) {
        sar_width = kSarTable[aspect_ratio_idc].sar_width;
        sar_height = kSarTable[aspect_ratio_idc].sar_height;
        has_sar = true;
      }
      ZX_DEBUG_ASSERT(aspect_ratio_idc != 0 || (!has_sar && sar_width == 1 && sar_height == 1));
      ZX_DEBUG_ASSERT(has_sar || (sar_width == 1 && sar_height == 1));
      ZX_DEBUG_ASSERT(sar_width != 0 && sar_height != 0);
    }
  }

  next_av_scratch0_ = (max_reference_size << 24) | (kActualDPBSize << 16) | (max_dpb_size << 8);

  // TODO(dustingreen): Plumb min and max frame counts, with max at least
  // kActualDPBSize (24 or higher if possible), and min sufficient to allow
  // decode to proceed without tending to leave the decoder idle for long if the
  // client immediately releases each frame (just barely enough to decode as
  // long as the client never camps on even one frame).
  zx_status_t status = InitializeFrames(kActualDPBSize, coded_width, coded_height, display_width,
                                        display_height, has_sar, sar_width, sar_height);
  if (status != ZX_OK) {
    if (status != ZX_ERR_STOP) {
      DECODE_ERROR("InitializeFrames() failed: status: %d\n", status);
    }
    return status;
  }

  return ZX_OK;
}

void H264Decoder::ReceivedFrames(uint32_t frame_count) {
  uint32_t error_count = AvScratchD::Get().ReadFrom(owner_->dosbus()).reg_value();
  // This hit_eos is _not_ the same as the is_end_of_stream in PtsOut below.
  bool hit_eos = false;
  for (uint32_t i = 0; i < frame_count && !hit_eos; i++) {
    auto pic_info = PicInfo::Get(i).ReadFrom(owner_->dosbus());
    uint32_t buffer_index = pic_info.buffer_index();
    uint32_t slice_type =
        (AvScratchH::Get().ReadFrom(owner_->dosbus()).reg_value() >> (i * 4)) & 0xf;
    if (pic_info.eos())
      hit_eos = true;

    // TODO(dustingreen): We'll need to bit-extend (nearest wins to allow for
    // re-ordering) this value to uint64_t, so that PTSs for frames after 4GiB
    // still work.
    uint32_t stream_byte_offset = pic_info.stream_offset();
    stream_byte_offset |=
        ((AvScratch::Get(0xa + i / 2).ReadFrom(owner_->dosbus()).reg_value() >> ((i % 2) * 16)) &
         0xffff)
        << 16;

    PtsManager::LookupResult pts_result = pts_manager_->Lookup(stream_byte_offset);
    video_frames_[buffer_index].frame->has_pts = pts_result.has_pts();
    video_frames_[buffer_index].frame->pts = pts_result.pts();
    if (pts_result.is_end_of_stream()) {
      // TODO(dustingreen): Handle this once we're able to detect this way.  For
      // now, ignore but print an obvious message.
      printf("##### UNHANDLED END OF STREAM DETECTED #####\n");
      break;
    }

    if (notifier_)
      notifier_(video_frames_[buffer_index].frame);
    DLOG("Got buffer %d error %d error_count %d slice_type %d offset %x\n", buffer_index,
         pic_info.error(), error_count, slice_type, pic_info.stream_offset());
  }
  AvScratch0::Get().FromValue(0).WriteTo(owner_->dosbus());
}

enum {
  kCommandInitializeStream = 1,
  kCommandNewFrames = 2,
  kCommandSwitchStreams = 3,
  kCommandFatalError = 6,
  kCommandGotFirstOffset = 9,
};

void H264Decoder::SwitchStreams() {
  // Signal that we're ready to allocate new frames for the new stream.
  AvScratch7::Get().FromValue(0).WriteTo(owner_->dosbus());
  AvScratch8::Get().FromValue(0).WriteTo(owner_->dosbus());
  AvScratch9::Get().FromValue(0).WriteTo(owner_->dosbus());

  // Signal firmware that command has been processed.
  AvScratch0::Get().FromValue(0).WriteTo(owner_->dosbus());
}

void H264Decoder::HandleInterrupt() {
  // Stop processing on fatal error.
  if (fatal_error_)
    return;

  VdecAssistMbox1ClrReg::Get().FromValue(1).WriteTo(owner_->dosbus());

  // Some returned frames may have been buffered up earlier, so try to return
  // them now that the firmware had a chance to do some work.
  TryReturnFrames();

  // The core signals the main processor what command to run using AvScratch0.
  // The main processor returns a result using AvScratch0 to trigger the decoder
  // to continue (possibly 0, if no result is needed).
  auto scratch0 = AvScratch0::Get().ReadFrom(owner_->dosbus());
  DLOG("Got command: %x", scratch0.reg_value());
  uint32_t cpu_command = scratch0.reg_value() & 0xff;
  switch (cpu_command) {
    case kCommandInitializeStream: {
      // For now, this can block for a while until buffers are allocated, or
      // until it fails. One of the ways it can fail is if the Codec client
      // closes the current stream at the Codec interface level (not exactly the
      // same thing as "stream" here).
      zx_status_t status = InitializeStream();
      if (status != ZX_OK) {
        OnFatalError();
      }
    } break;

    case kCommandNewFrames:
      ReceivedFrames((scratch0.reg_value() >> 8) & 0xff);
      break;

    case kCommandSwitchStreams:
      SwitchStreams();
      break;

    case kCommandFatalError: {
      auto error_count = AvScratchD::Get().ReadFrom(owner_->dosbus()).reg_value();
      DECODE_ERROR("Decoder fatal error %d\n", error_count);
      OnFatalError();
      // Don't write to AvScratch0, so the decoder won't continue.
      break;
    }

    case kCommandGotFirstOffset: {
      uint32_t first_offset = AvScratch1::Get().ReadFrom(owner_->dosbus()).reg_value();
      DLOG("First offset: %d\n", first_offset);
      AvScratch0::Get().FromValue(0).WriteTo(owner_->dosbus());
      break;
    }

    default:
      DECODE_ERROR("Got unknown command: %d\n", cpu_command);
      return;
  }

  auto sei_itu35_flags = AvScratchJ::Get().ReadFrom(owner_->dosbus()).reg_value();
  if (sei_itu35_flags & (1 << 15)) {
    DLOG("Got Supplemental Enhancement Information buffer");
    AvScratchJ::Get().FromValue(0).WriteTo(owner_->dosbus());
  }
}

void H264Decoder::OnFatalError() {
  if (!fatal_error_) {
    fatal_error_ = true;
    if (error_handler_) {
      error_handler_();
    }
  }
}
