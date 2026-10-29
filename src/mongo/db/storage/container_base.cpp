// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/container_base.h"

#include "mongo/util/assert_util.h"

namespace mongo {

IntegerKeyedContainerBase::IntegerKeyedContainerBase(std::shared_ptr<Ident> ident)
    : _ident(std::move(ident)) {}

std::shared_ptr<Ident> IntegerKeyedContainerBase::ident() const {
    return _ident;
}

void IntegerKeyedContainerBase::setIdent(std::shared_ptr<Ident> ident) {
    _ident = std::move(ident);
}

Status IntegerKeyedContainerBase::insert(RecoveryUnit& ru,
                                         std::span<const int64_t> keys,
                                         std::span<const std::span<const char>> values,
                                         container::ExistingKeyPolicy policy) {
    massert(13274500,
            "Spans for keys and values must have the same size",
            keys.size() == values.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        if (auto status = insert(ru, keys[i], values[i], policy); !status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

StringKeyedContainerBase::StringKeyedContainerBase(std::shared_ptr<Ident> ident)
    : _ident(std::move(ident)) {}

std::shared_ptr<Ident> StringKeyedContainerBase::ident() const {
    return _ident;
}

void StringKeyedContainerBase::setIdent(std::shared_ptr<Ident> ident) {
    _ident = std::move(ident);
}

Status StringKeyedContainerBase::insert(RecoveryUnit& ru,
                                        std::span<const std::span<const char>> keys,
                                        std::span<const std::span<const char>> values,
                                        container::ExistingKeyPolicy policy) {
    massert(13274501,
            "Spans for keys and values must have the same size",
            keys.size() == values.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        if (auto status = insert(ru, keys[i], values[i], policy); !status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

}  // namespace mongo
