// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file includes necessary methods for inspecting various on-disk structures
// of a MinFS filesystem.

#pragma once

#include <lib/disk-inspector/common-types.h>
#include <fbl/unique_fd.h>
#include <minfs/bcache.h>

namespace minfs {

class Inspector:public disk_inspector::DiskInspector {
public:
    Inspector() = delete;
    Inspector(const Inspector&)= delete;
    Inspector(Inspector&&) = delete;
    Inspector& operator=(const Inspector&) = delete;
    Inspector& operator=(Inspector&&) = delete;

    explicit Inspector(fbl::unique_fd fd) : fd_(std::move(fd)) {}

    // DiskInspector interface:
    zx_status_t GetRoot(std::unique_ptr<disk_inspector::DiskObject>* out) final;

private:
    // Creates root DiskObject.
    // Return ZX_OK on success.
    zx_status_t CreateRoot(std::unique_ptr<Bcache> bc,
                           std::unique_ptr<disk_inspector::DiskObject>* out);

    // File descriptor of the device to inspect.
    fbl::unique_fd fd_;
};

} // namespace minfs
