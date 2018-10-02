// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_LEDGER_APP_DISK_CLEANUP_MANAGER_IMPL_H_
#define PERIDOT_BIN_LEDGER_APP_DISK_CLEANUP_MANAGER_IMPL_H_

#include "peridot/bin/ledger/app/disk_cleanup_manager.h"
#include "peridot/bin/ledger/app/page_eviction_manager_impl.h"
#include "peridot/bin/ledger/app/page_usage_listener.h"
#include "peridot/bin/ledger/coroutine/coroutine.h"
#include "peridot/bin/ledger/filesystem/detached_path.h"

namespace ledger {

class DiskCleanupManagerImpl : public DiskCleanupManager {
 public:
  DiskCleanupManagerImpl(ledger::Environment* environment,
                         ledger::DetachedPath db_path);
  ~DiskCleanupManagerImpl() override;

  // Initializes this DiskCleanupManagerImpl.
  Status Init();

  // Sets the delegate for PageEvictionManager owned by DiskCleanupManagerImpl.
  // The delegate should outlive this object.
  void SetPageEvictionDelegate(PageEvictionManager::Delegate* delegate);

  // DiskCleanupManager:
  void set_on_empty(fit::closure on_empty_callback) override;
  bool IsEmpty() override;
  void TryCleanUp(fit::function<void(Status)> callback) override;
  void OnPageOpened(fxl::StringView ledger_name,
                    storage::PageIdView page_id) override;
  void OnPageClosed(fxl::StringView ledger_name,
                    storage::PageIdView page_id) override;

 private:
  PageEvictionManagerImpl page_eviction_manager_;
  std::unique_ptr<PageEvictionPolicy> policy_;

  // TODO(nellyv): Add OnLowResources and OnPeriodicCleanUp to handle cleanup
  // opeations on the corresponding cases.

  FXL_DISALLOW_COPY_AND_ASSIGN(DiskCleanupManagerImpl);
};

}  // namespace ledger

#endif  // PERIDOT_BIN_LEDGER_APP_DISK_CLEANUP_MANAGER_IMPL_H_
