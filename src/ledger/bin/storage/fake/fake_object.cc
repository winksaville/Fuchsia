// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ledger/bin/storage/fake/fake_object.h"

#include <utility>

namespace storage {
namespace fake {

FakePiece::FakePiece(ObjectIdentifier identifier, fxl::StringView content)
    : identifier_(std::move(identifier)), content_(content.ToString()) {}

fxl::StringView FakePiece::GetData() const { return content_; }

Status FakePiece::AppendReferences(ObjectReferencesAndPriority* references) const {
  return Status::OK;
}

ObjectIdentifier FakePiece::GetIdentifier() const { return identifier_; }

std::unique_ptr<const FakePiece> FakePiece::Clone() const {
  return std::make_unique<const FakePiece>(identifier_, content_);
}

FakeObject::FakeObject(ObjectIdentifier identifier, fxl::StringView content)
    : piece_(std::make_unique<FakePiece>(std::move(identifier), std::move(content))) {}

FakeObject::FakeObject(std::unique_ptr<const Piece> piece) : piece_(std::move(piece)) {}

ObjectIdentifier FakeObject::GetIdentifier() const { return piece_->GetIdentifier(); }

Status FakeObject::GetData(fxl::StringView* data) const {
  *data = piece_->GetData();
  return Status::OK;
}

Status FakeObject::AppendReferences(ObjectReferencesAndPriority* references) const {
  return Status::OK;
}

}  // namespace fake
}  // namespace storage
