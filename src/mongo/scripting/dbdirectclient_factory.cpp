// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/dbdirectclient_factory.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {

const ServiceContext::Decoration<DBDirectClientFactory> forService =
    ServiceContext::declareDecoration<DBDirectClientFactory>();

}  // namespace

DBDirectClientFactory& DBDirectClientFactory::get(ServiceContext* context) {
    fassert(40151, context);
    return forService(context);
}

DBDirectClientFactory& DBDirectClientFactory::get(OperationContext* opCtx) {
    fassert(40152, opCtx);
    return get(opCtx->getServiceContext());
}

void DBDirectClientFactory::registerImplementation(Impl implementation) {
    _implementation = std::move(implementation);
}

auto DBDirectClientFactory::create(OperationContext* opCtx) -> Result {
    uassert(40153, "Cannot create a direct client in this context", _implementation);
    return _implementation(opCtx);
}

}  // namespace mongo
