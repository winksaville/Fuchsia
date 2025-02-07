// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains a directory which contains blobs.

#ifndef ZIRCON_SYSTEM_ULIB_BLOBFS_DIRECTORY_H_
#define ZIRCON_SYSTEM_ULIB_BLOBFS_DIRECTORY_H_

#ifndef __Fuchsia__
#error Fuchsia-only Header
#endif

#include <fuchsia/blobfs/c/fidl.h>
#include <fuchsia/io/c/fidl.h>

#include <digest/digest.h>
#include <fbl/algorithm.h>
#include <fbl/ref_ptr.h>
#include <fs/vfs.h>
#include <fs/vfs_types.h>
#include <fs/vnode.h>

#include "blob-cache.h"

namespace blobfs {

class Blobfs;

using digest::Digest;
using BlockRegion = fuchsia_blobfs_BlockRegion;

// The root directory of blobfs. This directory is a flat container of all blobs in the filesystem.
class Directory final : public fs::Vnode {
 public:
  // Constructs the "directory" blob.
  Directory(Blobfs* bs);
  virtual ~Directory();
  DISALLOW_COPY_ASSIGN_AND_MOVE(Directory);

  // Shim to allow GetAllocatedRegions call to blobfs.
  zx_status_t GetAllocatedRegions(fidl_txn_t* txn) const;

 private:
  ////////////////
  // fs::Vnode interface.

  zx_status_t GetNodeInfoForProtocol(fs::VnodeProtocol protocol, fs::Rights rights,
                                     fs::VnodeRepresentation* info) final;
  fs::VnodeProtocolSet GetProtocols() const final;
  zx_status_t Readdir(fs::vdircookie_t* cookie, void* dirents, size_t len,
                      size_t* out_actual) final;
  zx_status_t Read(void* data, size_t len, size_t off, size_t* out_actual) final;
  zx_status_t Write(const void* data, size_t len, size_t offset, size_t* out_actual) final;
  zx_status_t Append(const void* data, size_t len, size_t* out_end, size_t* out_actual) final;
  zx_status_t Lookup(fbl::RefPtr<fs::Vnode>* out, fbl::StringPiece name) final;
  zx_status_t GetAttributes(fs::VnodeAttributes* a) final;
  zx_status_t Create(fbl::RefPtr<fs::Vnode>* out, fbl::StringPiece name, uint32_t mode) final;
  zx_status_t QueryFilesystem(fuchsia_io_FilesystemInfo* out) final;
  zx_status_t GetDevicePath(size_t buffer_len, char* out_name, size_t* out_len) final;
  zx_status_t Unlink(fbl::StringPiece name, bool must_be_dir) final;
  void Sync(SyncCallback closure) final;

#ifdef __Fuchsia__
  zx_status_t HandleFsSpecificMessage(fidl_msg_t* msg, fidl_txn_t* txn) final;
  static const fuchsia_blobfs_Blobfs_ops* Ops();
#endif

  ////////////////
  // Other methods.

  BlobCache& Cache();

  Blobfs* const blobfs_;
};

}  // namespace blobfs

#endif  // ZIRCON_SYSTEM_ULIB_BLOBFS_DIRECTORY_H_
