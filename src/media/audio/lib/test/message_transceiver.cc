// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/lib/test/message_transceiver.h"

namespace media::audio::test {

MessageTransceiver::MessageTransceiver(async_dispatcher_t* dispatcher) : dispatcher_(dispatcher) {}

MessageTransceiver::~MessageTransceiver() { Close(); };

zx_status_t MessageTransceiver::Init(zx::channel channel,
                                     IncomingMessageCallback incoming_message_callback,
                                     ErrorCallback error_callback) {
  channel_ = std::move(channel);
  incoming_message_callback_ = std::move(incoming_message_callback);
  error_callback_ = std::move(error_callback);

  wait_.set_object(channel_.get());
  wait_.set_trigger(ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED);

  return wait_.Begin(dispatcher_);
}

void MessageTransceiver::Close() {
  wait_.Cancel();
  channel_.reset();
  incoming_message_callback_ = nullptr;
  error_callback_ = nullptr;
}

zx_status_t MessageTransceiver::SendMessage(Message message) {
  if (!channel_) {
    return ZX_ERR_NOT_CONNECTED;
  }

  zx_status_t status = channel_.write(0, message.bytes_.data(), message.bytes_.size(),
                                      message.handles_.data(), message.handles_.size());
  if (status != ZX_OK) {
    FXL_PLOG(ERROR, status) << "zx::channel::write failed";
    OnError(status);
    return status;
  }
  return ZX_OK;
}

void MessageTransceiver::OnError(zx_status_t status) {
  if (error_callback_) {
    error_callback_(status);
  }

  Close();
}

void MessageTransceiver::ReadChannelMessages(async_dispatcher_t* dispatcher, async::WaitBase* wait,
                                             zx_status_t status, const zx_packet_signal_t* signal) {
  while (channel_) {
    uint32_t actual_byte_count;
    uint32_t actual_handle_count;
    zx_status_t status =
        channel_.read(0, nullptr, nullptr, 0, 0, &actual_byte_count, &actual_handle_count);

    if (status == ZX_ERR_SHOULD_WAIT) {
      status = wait->Begin(dispatcher);
      if (status != ZX_OK) {
        FXL_PLOG(ERROR, status) << "async::WaitMethod::Begin failed";
        OnError(status);
      }
      break;
    }

    if (status == ZX_ERR_PEER_CLOSED) {
      // Remote end of the channel closed.
      OnError(status);
      break;
    }

    if (status != ZX_ERR_BUFFER_TOO_SMALL) {
      FXL_PLOG(ERROR, status) << "Failed to read (peek) from a zx::channel";
      OnError(status);
      break;
    }

    Message message(actual_byte_count, actual_handle_count);
    status = channel_.read(0, message.bytes_.data(), message.handles_.data(), message.bytes_.size(),
                           message.handles_.size(), &actual_byte_count, &actual_handle_count);

    if (status != ZX_OK) {
      FXL_PLOG(ERROR, status) << "zx::channel::read failed";
      OnError(status);
      break;
    }

    FXL_CHECK(message.bytes_.size() == actual_byte_count);
    FXL_CHECK(message.handles_.size() == actual_handle_count);

    if (incoming_message_callback_) {
      incoming_message_callback_(std::move(message));
    }
  }
}

}  // namespace media::audio::test
