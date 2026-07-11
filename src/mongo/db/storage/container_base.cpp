// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/container_base.h"

namespace mongo {

IntegerKeyedContainerBase::IntegerKeyedContainerBase(std::shared_ptr<Ident> ident)
    : _ident(std::move(ident)) {}

std::shared_ptr<Ident> IntegerKeyedContainerBase::ident() const {
    return _ident;
}

void IntegerKeyedContainerBase::setIdent(std::shared_ptr<Ident> ident) {
    _ident = std::move(ident);
}

StringKeyedContainerBase::StringKeyedContainerBase(std::shared_ptr<Ident> ident)
    : _ident(std::move(ident)) {}

std::shared_ptr<Ident> StringKeyedContainerBase::ident() const {
    return _ident;
}

void StringKeyedContainerBase::setIdent(std::shared_ptr<Ident> ident) {
    _ident = std::move(ident);
}

}  // namespace mongo
