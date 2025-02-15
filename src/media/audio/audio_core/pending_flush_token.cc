// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/audio_core/pending_flush_token.h"

#include <lib/async/cpp/task.h>

#include <trace/event.h>

#include "src/lib/fxl/logging.h"
#include "src/media/audio/audio_core/audio_core_impl.h"

namespace media::audio {

void PendingFlushToken::fbl_recycle() {
  TRACE_DURATION("audio", "PendingFlushToken::fbl_recycle");
  if (callback_) {
    async::PostTask(dispatcher_, std::move(callback_));
  }
  delete this;
}

}  // namespace media::audio
