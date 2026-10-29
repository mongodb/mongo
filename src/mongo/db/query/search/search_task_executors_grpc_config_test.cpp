// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/query/search/mongot_options.h"
#include "mongo/db/query/search/search_task_executors.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(SearchTaskExecutors, UassertsWhenGrpcEnabledWithoutGrpcSupport) {
#ifndef MONGO_CONFIG_GRPC
    const bool oldUseGrpc = globalMongotParams.useGRPC;
    globalMongotParams.useGRPC = true;

    ASSERT_THROWS_CODE(ServiceContext::make(), DBException, ErrorCodes::InvalidOptions);

    globalMongotParams.useGRPC = oldUseGrpc;
#endif
}

}  // namespace
}  // namespace mongo


