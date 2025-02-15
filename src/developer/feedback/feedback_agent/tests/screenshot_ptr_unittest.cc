// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/feedback/feedback_agent/screenshot_ptr.h"

#include <fuchsia/ui/scenic/cpp/fidl.h>
#include <lib/async/cpp/executor.h>
#include <lib/fit/single_threaded_executor.h>
#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/testing/service_directory_provider.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include <memory>
#include <vector>

#include "src/developer/feedback/feedback_agent/tests/stub_scenic.h"
#include "src/lib/fxl/logging.h"
#include "src/lib/fxl/test/test_settings.h"
#include "src/lib/syslog/cpp/logger.h"
#include "third_party/googletest/googlemock/include/gmock/gmock.h"
#include "third_party/googletest/googletest/include/gtest/gtest.h"

namespace feedback {
namespace {

using fuchsia::ui::scenic::ScreenshotData;

constexpr bool kSuccess = true;

class TakeScreenshotTest : public gtest::TestLoopFixture {
 public:
  TakeScreenshotTest() : executor_(dispatcher()), service_directory_provider_(dispatcher()) {}

 protected:
  void SetUpScenic(std::unique_ptr<StubScenic> stub_scenic) {
    stub_scenic_ = std::move(stub_scenic);
    if (stub_scenic_) {
      FXL_CHECK(service_directory_provider_.AddService(stub_scenic_->GetHandler()) == ZX_OK);
    }
  }

  fit::result<ScreenshotData> TakeScreenshot(const zx::duration timeout = zx::sec(1)) {
    fit::result<ScreenshotData> result;
    executor_.schedule_task(
        feedback::TakeScreenshot(dispatcher(), service_directory_provider_.service_directory(),
                                 timeout)
            .then([&result](fit::result<ScreenshotData>& res) { result = std::move(res); }));
    RunLoopFor(timeout);
    return result;
  }

  async::Executor executor_;
  sys::testing::ServiceDirectoryProvider service_directory_provider_;

 private:
  std::unique_ptr<StubScenic> stub_scenic_;
};

TEST_F(TakeScreenshotTest, Succeed_CheckerboardScreenshot) {
  const size_t image_dim_in_px = 100;
  std::vector<TakeScreenshotResponse> scenic_responses;
  scenic_responses.emplace_back(CreateCheckerboardScreenshot(image_dim_in_px), kSuccess);
  std::unique_ptr<StubScenic> stub_scenic = std::make_unique<StubScenic>();
  stub_scenic->set_take_screenshot_responses(std::move(scenic_responses));
  SetUpScenic(std::move(stub_scenic));

  fit::result<ScreenshotData> result = TakeScreenshot();

  ASSERT_TRUE(result.is_ok());
  ScreenshotData screenshot = result.take_value();
  EXPECT_TRUE(screenshot.data.vmo.is_valid());
  EXPECT_EQ(static_cast<size_t>(screenshot.info.height), image_dim_in_px);
  EXPECT_EQ(static_cast<size_t>(screenshot.info.width), image_dim_in_px);
  EXPECT_EQ(screenshot.info.stride, image_dim_in_px * 4u);
  EXPECT_EQ(screenshot.info.pixel_format, fuchsia::images::PixelFormat::BGRA_8);
}

TEST_F(TakeScreenshotTest, Fail_ScenicNotAvailable) {
  SetUpScenic(nullptr);

  fit::result<ScreenshotData> result = TakeScreenshot();

  ASSERT_TRUE(result.is_error());
}

TEST_F(TakeScreenshotTest, Fail_ScenicReturningFalse) {
  SetUpScenic(std::make_unique<StubScenicAlwaysReturnsFalse>());

  fit::result<ScreenshotData> result = TakeScreenshot();

  ASSERT_TRUE(result.is_error());
}

TEST_F(TakeScreenshotTest, Fail_ScenicClosesConnection) {
  SetUpScenic(std::make_unique<StubScenicClosesConnection>());

  fit::result<ScreenshotData> result = TakeScreenshot();

  ASSERT_TRUE(result.is_error());
}

TEST_F(TakeScreenshotTest, Fail_ScenicNeverReturns) {
  SetUpScenic(std::make_unique<StubScenicNeverReturns>());

  fit::result<ScreenshotData> result = TakeScreenshot();

  ASSERT_TRUE(result.is_error());
}

TEST_F(TakeScreenshotTest, Fail_CallTakeScreenshotTwice) {
  std::vector<TakeScreenshotResponse> scenic_responses;
  scenic_responses.emplace_back(CreateEmptyScreenshot(), kSuccess);
  std::unique_ptr<StubScenic> stub_scenic = std::make_unique<StubScenic>();
  stub_scenic->set_take_screenshot_responses(std::move(scenic_responses));
  SetUpScenic(std::move(stub_scenic));

  const zx::duration unused_timeout = zx::sec(1);
  Scenic scenic(dispatcher(), service_directory_provider_.service_directory());
  executor_.schedule_task(scenic.TakeScreenshot(unused_timeout));
  ASSERT_DEATH(scenic.TakeScreenshot(unused_timeout),
               testing::HasSubstr("TakeScreenshot() is not intended to be called twice"));
}

}  // namespace
}  // namespace feedback

int main(int argc, char** argv) {
  if (!fxl::SetTestSettings(argc, argv)) {
    return EXIT_FAILURE;
  }

  testing::InitGoogleTest(&argc, argv);
  syslog::InitLogger({"feedback", "test"});
  return RUN_ALL_TESTS();
}
