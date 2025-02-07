// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/kazoo/outputs.h"
#include "tools/kazoo/syscall_library.h"
#include "tools/kazoo/test.h"
#include "tools/kazoo/test_ir_test_rust_selection.test.h"

namespace {

TEST(RustOutput, Various) {
  SyscallLibrary library;
  ASSERT_TRUE(SyscallLibraryLoader::FromJson(k_test_rust_selection, &library));

  StringWriter writer;
  ASSERT_TRUE(RustOutput(library, &writer));

  EXPECT_EQ(writer.Out(),
            R"(// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// WARNING: THIS FILE IS MACHINE GENERATED BY //tools/kazoo. DO NOT EDIT.

#[link(name = "zircon")]
extern {
    pub fn zx_rust_simple_case(
        ) -> zx_time_t;

    pub fn zx_rust_multiple_in_handles(
        handles: *const zx_handle_t,
        num_handles: usize
        ) -> zx_status_t;

    pub fn zx_rust_ano_ret_func(
        );

    pub fn zx_rust_no_return_value(
        x: u32
        );

    pub fn zx_rust_inout_args(
        handle: zx_handle_t,
        op: u32,
        offset: u64,
        size: u64,
        buffer: *mut u8,
        buffer_size: usize
        ) -> zx_status_t;

    pub fn zx_rust_const_input(
        input: *const u8,
        num_input: usize
        ) -> zx_status_t;

    pub fn zx_rust_various_basic_type_names(
        a: bool,
        b: u8,
        d: i32,
        e: i64,
        f: u16,
        g: u32,
        h: u64,
        i: usize,
        j: usize,
        k: *mut u8,
        l: zx_time_t,
        m: zx_ticks_t
        );


}
)");
}

}  // namespace
