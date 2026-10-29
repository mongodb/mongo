// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/grpc_session_manager.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/grpc/client_cache.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {
namespace {

class GRPCSessionManagerTest : public ServiceContextTest {};

/**
 * Verifies that GRPCSessionManager does not opt in to the "connections" serverStatus section
 * (it reports under the "gRPC" section instead).
 */
TEST_F(GRPCSessionManagerTest, DoesNotReportToConnectionsServerStatusSection) {
    auto clientCache = std::make_shared<ClientCache>();
    GRPCSessionManager sm(getServiceContext(), clientCache);
    ASSERT_FALSE(sm.shouldIncludeInConnectionsServerStatus());
}

}  // namespace
}  // namespace mongo::transport::grpc
