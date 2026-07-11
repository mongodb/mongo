// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/util/modules.h"

namespace mongo {

class OperationContext;

class [[MONGO_MOD_OPEN]] MongoProcessInterfaceFactory {
public:
    virtual ~MongoProcessInterfaceFactory() = default;
    virtual std::shared_ptr<MongoProcessInterface> create(OperationContext* opCtx) = 0;
};

class [[MONGO_MOD_OPEN]] MongoProcessInterfaceFactoryImpl : public MongoProcessInterfaceFactory {
public:
    std::shared_ptr<MongoProcessInterface> create(OperationContext* opCtx) override;
};

}  // namespace mongo
