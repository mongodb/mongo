// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/logical_session_cache.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {
const auto getLogicalSessionCache =
    ServiceContext::declareDecoration<std::unique_ptr<LogicalSessionCache>>();

const auto getLogicalSessionCacheIsRegistered = ServiceContext::declareDecoration<Atomic<bool>>();
}  // namespace

LogicalSessionCache::~LogicalSessionCache() = default;

LogicalSessionCache* LogicalSessionCache::get(ServiceContext* service) {
    if (getLogicalSessionCacheIsRegistered(service).load()) {
        return getLogicalSessionCache(service).get();
    }
    return nullptr;
}

LogicalSessionCache* LogicalSessionCache::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void LogicalSessionCache::set(ServiceContext* service,
                              std::unique_ptr<LogicalSessionCache> sessionCache) {
    auto& cache = getLogicalSessionCache(service);
    cache = std::move(sessionCache);
    getLogicalSessionCacheIsRegistered(service).store(true);
}

}  // namespace mongo
