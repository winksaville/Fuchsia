// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/callback/waiter.h"

#include <lib/fit/defer.h>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "src/lib/callback/capture.h"
#include "src/lib/callback/set_when_called.h"

namespace callback {
namespace {

using ::testing::ElementsAre;

TEST(Waiter, NoCallback) {
  auto waiter = fxl::MakeRefCounted<Waiter<int, int>>(0);

  int result = -1;
  std::vector<int> data;
  waiter->Finalize(Capture([] {}, &result, &data));

  EXPECT_EQ(0, result);
  EXPECT_EQ(std::vector<int>(), data);
}

TEST(Waiter, DataPreInitialize) {
  auto waiter = fxl::MakeRefCounted<Waiter<int, int>>(0);

  waiter->NewCallback()(0, 0);
  waiter->NewCallback()(0, 1);
  waiter->NewCallback()(0, 2);

  int result = -1;
  std::vector<int> data;
  waiter->Finalize(Capture([] {}, &result, &data));

  EXPECT_EQ(0, result);
  EXPECT_EQ(std::vector<int>({0, 1, 2}), data);
}

TEST(Waiter, DataPostInitialize) {
  auto waiter = fxl::MakeRefCounted<Waiter<int, int>>(0);

  auto c1 = waiter->NewCallback();
  auto c2 = waiter->NewCallback();
  auto c3 = waiter->NewCallback();

  int result = -1;
  std::vector<int> data;
  waiter->Finalize(Capture([] {}, &result, &data));

  EXPECT_EQ(-1, result);
  c1(0, 0);
  EXPECT_EQ(-1, result);
  c2(0, 1);
  EXPECT_EQ(-1, result);
  c3(0, 2);

  EXPECT_EQ(0, result);
  EXPECT_EQ(std::vector<int>({0, 1, 2}), data);
}

TEST(Waiter, DataMixedInitialize) {
  auto waiter = fxl::MakeRefCounted<Waiter<int, int>>(0);

  waiter->NewCallback()(0, 0);
  waiter->NewCallback()(0, 1);

  auto c = waiter->NewCallback();

  int result = -1;
  std::vector<int> data;
  waiter->Finalize(Capture([] {}, &result, &data));

  EXPECT_EQ(-1, result);

  c(0, 2);

  EXPECT_EQ(0, result);
  EXPECT_EQ(std::vector<int>({0, 1, 2}), data);
}

TEST(Waiter, UnorderedCalls) {
  auto waiter = fxl::MakeRefCounted<Waiter<int, int>>(0);

  auto c1 = waiter->NewCallback();
  auto c2 = waiter->NewCallback();
  auto c3 = waiter->NewCallback();

  c2(0, 1);
  c3(0, 2);
  c1(0, 0);

  int result = -1;
  std::vector<int> data;
  waiter->Finalize(Capture([] {}, &result, &data));

  EXPECT_EQ(0, result);
  EXPECT_EQ(std::vector<int>({0, 1, 2}), data);
}

TEST(Waiter, EarlyReturnOnError) {
  auto waiter = fxl::MakeRefCounted<Waiter<int, int>>(0);

  waiter->NewCallback();
  waiter->NewCallback()(1, 2);
  waiter->NewCallback();

  int result = -1;
  std::vector<int> data;
  waiter->Finalize(Capture([] {}, &result, &data));

  EXPECT_EQ(1, result);
  EXPECT_EQ(std::vector<int>(), data);
}

TEST(Waiter, CallbackSurviveWaiter) {
  auto waiter = fxl::MakeRefCounted<Waiter<int, int>>(0);
  auto c1 = waiter->NewCallback();

  waiter = nullptr;

  c1(0, 0);
}

TEST(Waiter, MultipleParameterCallack) {
  auto waiter = fxl::MakeRefCounted<Waiter<int, int, int>>(0);
  auto c1 = waiter->NewCallback();
  c1(0, 1, 2);

  std::vector<std::tuple<int, int>> data;
  int result = -1;
  waiter->Finalize(Capture([] {}, &result, &data));

  EXPECT_EQ(0, result);
  EXPECT_THAT(data, ElementsAre(std::make_tuple(1, 2)));
}

TEST(Waiter, Promise) {
  auto promise = fxl::MakeRefCounted<Promise<int, int>>(0);

  promise->NewCallback()(1, 2);
  int status, result;
  promise->Finalize(Capture([] {}, &status, &result));
  EXPECT_EQ(1, status);
  EXPECT_EQ(2, result);
}

TEST(Waiter, DeleteInFinalize) {
  auto promise = fxl::MakeRefCounted<Promise<int, int>>(0);
  promise->NewCallback()(1, 2);
  promise->Finalize([&](int status, int result) {
    // Delete the callback.
    promise = nullptr;
  });
}

TEST(StatusWaiter, MixedInitialize) {
  auto waiter = fxl::MakeRefCounted<StatusWaiter<int>>(0);

  waiter->NewCallback()(0);
  waiter->NewCallback()(0);
  auto c = waiter->NewCallback();

  int result = -1;
  waiter->Finalize(Capture([] {}, &result));

  EXPECT_EQ(-1, result);

  c(0);
  EXPECT_EQ(0, result);
}

TEST(StatusWaiter, EarlyReturnOnError) {
  auto waiter = fxl::MakeRefCounted<StatusWaiter<int>>(0);

  waiter->NewCallback()(0);
  waiter->NewCallback()(1);

  // Create callback, but do not call it.
  waiter->NewCallback();

  int result = -1;
  waiter->Finalize(Capture([] {}, &result));

  EXPECT_EQ(1, result);
}

TEST(CompletionWaiter, MixedInitialize) {
  auto waiter = fxl::MakeRefCounted<CompletionWaiter>();

  waiter->NewCallback()();
  waiter->NewCallback()();
  auto c = waiter->NewCallback();

  bool called = false;
  waiter->Finalize([&called] { called = true; });

  EXPECT_FALSE(called);

  c();

  EXPECT_TRUE(called);
}

TEST(Waiter, CancelThenFinalize) {
  auto waiter = fxl::MakeRefCounted<CompletionWaiter>();

  auto callback = waiter->NewCallback();

  waiter->Cancel();

  bool called = false;
  waiter->Finalize([&called] { called = true; });

  EXPECT_FALSE(called);
  callback();
  EXPECT_FALSE(called);
}

TEST(Waiter, FinalizeThenCancel) {
  auto waiter = fxl::MakeRefCounted<CompletionWaiter>();

  auto callback = waiter->NewCallback();

  bool called = false;
  waiter->Finalize([&called] { called = true; });

  EXPECT_FALSE(called);
  waiter->Cancel();
  callback();
  EXPECT_FALSE(called);
}

TEST(Waiter, CancelDeletesCallback) {
  auto waiter = fxl::MakeRefCounted<CompletionWaiter>();

  auto callback = waiter->NewCallback();

  bool called = false;
  auto on_destruction = fit::defer(callback::SetWhenCalled(&called));
  waiter->Finalize([on_destruction = std::move(on_destruction)] {});

  EXPECT_FALSE(called);
  waiter->Cancel();
  EXPECT_TRUE(called);
}

TEST(Waiter, FinalizeDeletesCallback) {
  auto waiter = fxl::MakeRefCounted<CompletionWaiter>();

  auto callback = waiter->NewCallback();

  bool called = false;
  auto on_destruction = fit::defer(callback::SetWhenCalled(&called));
  waiter->Finalize([on_destruction = std::move(on_destruction)] {});

  EXPECT_FALSE(called);
  callback();
  EXPECT_TRUE(called);
}

TEST(AnyWaiter, FailureThenSuccess) {
  auto waiter = fxl::MakeRefCounted<AnyWaiter<bool, int>>(true, false);

  auto cb1 = waiter->NewCallback();
  auto cb2 = waiter->NewCallback();
  auto cb3 = waiter->NewCallback();
  bool called;
  int status, result;
  waiter->Finalize(Capture(SetWhenCalled(&called), &status, &result));
  EXPECT_FALSE(called);
  cb1(false, 1);
  EXPECT_FALSE(called);
  cb2(true, 2);
  EXPECT_TRUE(called);
  EXPECT_EQ(true, status);
  EXPECT_EQ(2, result);

  called = false;
  cb3(true, 2);
  EXPECT_FALSE(called);
}

TEST(AnyWaiter, AllFailure) {
  auto waiter = fxl::MakeRefCounted<AnyWaiter<bool, int>>(true, false, -1);

  auto cb1 = waiter->NewCallback();
  auto cb2 = waiter->NewCallback();
  auto cb3 = waiter->NewCallback();
  bool called;
  int status, result;
  waiter->Finalize(Capture(SetWhenCalled(&called), &status, &result));
  EXPECT_FALSE(called);
  cb1(false, 1);
  EXPECT_FALSE(called);
  cb2(false, 2);
  EXPECT_FALSE(called);
  cb3(false, 3);
  EXPECT_TRUE(called);
  EXPECT_EQ(false, status);
  EXPECT_EQ(-1, result);
}

TEST(AnyWaiter, Default) {
  auto waiter = fxl::MakeRefCounted<AnyWaiter<bool, int>>(true, false, -1);

  bool called;
  int status, result;
  waiter->Finalize(Capture(SetWhenCalled(&called), &status, &result));
  EXPECT_TRUE(called);
  EXPECT_EQ(false, status);
  EXPECT_EQ(-1, result);
}

TEST(StatusWaiter, ScopedSuccess) {
  bool scoped1_called;
  bool scoped2_called;
  bool finalized;
  bool status;

  auto waiter = fxl::MakeRefCounted<StatusWaiter<bool>>(true);
  auto callback = waiter->NewCallback();
  auto scoped1 = waiter->MakeScoped(SetWhenCalled(&scoped1_called));
  auto scoped2 = waiter->MakeScoped(SetWhenCalled(&scoped2_called));
  waiter->Finalize(Capture(SetWhenCalled(&finalized), &status));

  scoped1();
  EXPECT_TRUE(scoped1_called);

  callback(true);
  ASSERT_TRUE(finalized);
  EXPECT_EQ(status, true);

  scoped2();
  EXPECT_FALSE(scoped2_called);
}

TEST(StatusWaiter, ScopedFailure) {
  bool scoped1_called;
  bool scoped2_called;
  bool finalized;
  bool status;

  auto waiter = fxl::MakeRefCounted<StatusWaiter<bool>>(true);
  auto callback1 = waiter->NewCallback();
  auto callback2 = waiter->NewCallback();
  auto scoped1 = waiter->MakeScoped(SetWhenCalled(&scoped1_called));
  auto scoped2 = waiter->MakeScoped(SetWhenCalled(&scoped2_called));
  waiter->Finalize(Capture(SetWhenCalled(&finalized), &status));

  scoped1();
  EXPECT_TRUE(scoped1_called);

  callback1(false);
  ASSERT_TRUE(finalized);
  EXPECT_EQ(status, false);

  scoped2();
  EXPECT_FALSE(scoped2_called);
}

TEST(StatusWaiter, ScopedCancelled) {
  bool scoped1_called;
  bool scoped2_called;
  bool finalized;
  bool status;

  auto waiter = fxl::MakeRefCounted<StatusWaiter<bool>>(true);
  auto callback = waiter->NewCallback();
  auto scoped1 = waiter->MakeScoped(SetWhenCalled(&scoped1_called));
  auto scoped2 = waiter->MakeScoped(SetWhenCalled(&scoped2_called));
  waiter->Finalize(Capture(SetWhenCalled(&finalized), &status));

  scoped1();
  EXPECT_TRUE(scoped1_called);

  waiter->Cancel();
  ASSERT_FALSE(finalized);

  scoped2();
  EXPECT_FALSE(scoped2_called);
}

}  //  namespace
}  //  namespace callback
