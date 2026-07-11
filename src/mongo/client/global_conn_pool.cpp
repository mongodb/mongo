// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/global_conn_pool.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/client/global_conn_pool_gen.h"

#include <string>

namespace mongo {
namespace {

MONGO_INITIALIZER_WITH_PREREQUISITES(InitializeGlobalConnectionPool, ("EndStartupOptionStorage"))
(InitializerContext* context) {
    globalConnPool.setName("connection pool");
    globalConnPool.setMaxPoolSize(maxConnsPerHost);
    globalConnPool.setMaxInUse(maxInUseConnsPerHost);
    globalConnPool.setIdleTimeout(globalConnPoolIdleTimeout);
}

}  // namespace

DBConnectionPool globalConnPool;

}  // namespace mongo
