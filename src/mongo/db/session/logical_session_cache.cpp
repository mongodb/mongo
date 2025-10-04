/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/session/logical_session_cache.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/decorable.h"

#include <utility>

namespace mongo {

namespace {
const auto getLogicalSessionCache =
    ServiceContext::declareDecoration<std::unique_ptr<LogicalSessionCache>>();

const auto getLogicalSessionCacheIsRegistered =
    ServiceContext::declareDecoration<AtomicWord<bool>>();
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
