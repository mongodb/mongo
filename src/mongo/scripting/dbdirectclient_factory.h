// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/dbclient_base.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>

namespace mongo {

class OperationContext;
class ServiceContext;

class [[MONGO_MOD_PUBLIC]] DBDirectClientFactory {
public:
    using Result = std::unique_ptr<DBClientBase>;
    using Impl = std::function<Result(OperationContext*)>;

    static DBDirectClientFactory& get(ServiceContext* service);
    static DBDirectClientFactory& get(OperationContext* opCtx);

    void registerImplementation(Impl implementation);
    Result create(OperationContext* opCtx);

private:
    Impl _implementation;
};

}  // namespace mongo
