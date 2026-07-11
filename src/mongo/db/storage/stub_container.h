// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/container_base.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class StubIntegerKeyedContainer final : public IntegerKeyedContainerBase {
public:
    StubIntegerKeyedContainer() : IntegerKeyedContainerBase(nullptr) {}
    Status insert(RecoveryUnit& ru,
                  int64_t key,
                  std::span<const char> value,
                  container::ExistingKeyPolicy policy) final {
        return Status::OK();
    }
    Status update(RecoveryUnit& ru, int64_t key, std::span<const char> value) final {
        return Status::OK();
    }
    Status remove(RecoveryUnit& ru, int64_t key) final {
        return Status::OK();
    }
    std::unique_ptr<Cursor> getCursor(RecoveryUnit& ru) const final {
        return nullptr;
    }
};

class StubStringKeyedContainer final : public StringKeyedContainerBase {
public:
    StubStringKeyedContainer() : StringKeyedContainerBase(nullptr) {}
    Status insert(RecoveryUnit& ru,
                  std::span<const char> key,
                  std::span<const char> value,
                  container::ExistingKeyPolicy policy) final {
        return Status::OK();
    }
    Status update(RecoveryUnit& ru, std::span<const char> key, std::span<const char> value) final {
        return Status::OK();
    }
    Status remove(RecoveryUnit& ru, std::span<const char> key) final {
        return Status::OK();
    }
    std::unique_ptr<Cursor> getCursor(RecoveryUnit& ru) const final {
        return nullptr;
    }
};

}  // namespace mongo
