// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_H_
#define SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_H_

#include <optional>

#include "src/lib/fxl/logging.h"
#include "src/media/audio/audio_core/pipeline_config.h"
#include "src/media/audio/audio_core/volume_curve.h"

namespace media::audio {

class ProcessConfig;

class ProcessConfigBuilder {
 public:
  ProcessConfigBuilder& SetDefaultVolumeCurve(VolumeCurve curve);
  ProcessConfigBuilder& AddOutputStreamEffects(PipelineConfig::MixGroup effects);
  ProcessConfigBuilder& SetMixEffects(PipelineConfig::MixGroup effects);
  ProcessConfigBuilder& SetLinearizeEffects(PipelineConfig::MixGroup effects);
  ProcessConfig Build();

 private:
  PipelineConfig pipeline_;
  std::optional<VolumeCurve> default_volume_curve_;
};

class ProcessConfig {
 public:
  class HandleImpl {
   public:
    ~HandleImpl() { ProcessConfig::instance_ = {}; }

    // Disallow copy/move.
    HandleImpl(const HandleImpl&) = delete;
    HandleImpl& operator=(const HandleImpl&) = delete;
    HandleImpl(HandleImpl&& o) = delete;
    HandleImpl& operator=(HandleImpl&& o) = delete;

   private:
    friend class ProcessConfig;
    HandleImpl() = default;
  };
  using Handle = std::unique_ptr<HandleImpl>;

  // Sets the |ProcessConfig|.
  //
  // |ProcessConfig::instance()| will return a reference to |config| as long as the returned
  // |ProcessConfig::Handle| exists. It's illegal to call |set_instance| while a
  // |ProcessConfig::Handle| is active.
  [[nodiscard]] static ProcessConfig::Handle set_instance(ProcessConfig config) {
    FXL_CHECK(!ProcessConfig::instance_);
    ProcessConfig::instance_ = {std::move(config)};
    return std::unique_ptr<HandleImpl>(new HandleImpl);
  }
  // Returns the |ProcessConfig|. Must be called while there is a
  static const ProcessConfig& instance() {
    FXL_CHECK(ProcessConfig::instance_);
    return *ProcessConfig::instance_;
  }

  using Builder = ProcessConfigBuilder;
  ProcessConfig(VolumeCurve curve, PipelineConfig effects)
      : default_volume_curve_(std::move(curve)), pipeline_(std::move(effects)) {}

  const VolumeCurve& default_volume_curve() const { return default_volume_curve_; }
  const PipelineConfig& pipeline() const { return pipeline_; }

 private:
  static std::optional<ProcessConfig> instance_;

  VolumeCurve default_volume_curve_;
  PipelineConfig pipeline_;
};

}  // namespace media::audio

#endif  // SRC_MEDIA_AUDIO_AUDIO_CORE_PROCESS_CONFIG_H_
