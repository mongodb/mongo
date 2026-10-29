// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/dbcheck/health_log_interface.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {
const auto getHealthLog = ServiceContext::declareDecoration<std::unique_ptr<HealthLogInterface>>();
}  // namespace

void HealthLogInterface::set(ServiceContext* serviceContext,
                             std::unique_ptr<HealthLogInterface> newHealthLog) {
    auto& healthLog = getHealthLog(serviceContext);
    invariant(!healthLog);

    healthLog = std::move(newHealthLog);
}

HealthLogInterface* HealthLogInterface::get(ServiceContext* svcCtx) {
    return getHealthLog(svcCtx).get();
}

HealthLogInterface* HealthLogInterface::get(OperationContext* opCtx) {
    return getHealthLog(opCtx->getServiceContext()).get();
}
}  // namespace mongo
