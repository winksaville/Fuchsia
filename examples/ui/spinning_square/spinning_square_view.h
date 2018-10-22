// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_EXAMPLES_UI_SPINNING_SQUARE_SPINNING_SQUARE_VIEW_H_
#define GARNET_EXAMPLES_UI_SPINNING_SQUARE_SPINNING_SQUARE_VIEW_H_

#include "lib/fxl/macros.h"
#include "lib/ui/base_view/cpp/v1_base_view.h"
#include "lib/ui/scenic/cpp/resources.h"

namespace examples {

class SpinningSquareView : public scenic::V1BaseView {
 public:
  SpinningSquareView(scenic::ViewContext context);
  ~SpinningSquareView() override;

 private:
  // | scenic::V1BaseView |
  void OnSceneInvalidated(
      fuchsia::images::PresentationInfo presentation_info) override;

  scenic::ShapeNode background_node_;
  scenic::ShapeNode square_node_;

  uint64_t start_time_ = 0u;

  FXL_DISALLOW_COPY_AND_ASSIGN(SpinningSquareView);
};

}  // namespace examples

#endif  // GARNET_EXAMPLES_UI_SPINNING_SQUARE_SPINNING_SQUARE_VIEW_H_
