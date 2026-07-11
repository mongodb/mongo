// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/container.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_OPEN]] IntegerKeyedContainerBase : public IntegerKeyedContainer {
public:
    explicit IntegerKeyedContainerBase(std::shared_ptr<Ident> ident);

    std::shared_ptr<Ident> ident() const final;

    void setIdent(std::shared_ptr<Ident> ident) final;

private:
    std::shared_ptr<Ident> _ident;
};

class [[MONGO_MOD_OPEN]] StringKeyedContainerBase : public StringKeyedContainer {
public:
    explicit StringKeyedContainerBase(std::shared_ptr<Ident> ident);

    std::shared_ptr<Ident> ident() const final;

    void setIdent(std::shared_ptr<Ident> ident) final;

private:
    std::shared_ptr<Ident> _ident;
};

}  // namespace mongo
