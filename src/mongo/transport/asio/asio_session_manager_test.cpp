// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/asio/asio_session_manager.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport {
namespace {

class AsioSessionManagerTest : public ServiceContextTest {};

/**
 * Verifies that AsioSessionManager opts in to the "connections" serverStatus section.
 */
TEST_F(AsioSessionManagerTest, ShouldIncludeInConnectionsServerStatus) {
    AsioSessionManager sm(getServiceContext());
    ASSERT_TRUE(sm.shouldIncludeInConnectionsServerStatus());
}

}  // namespace
}  // namespace mongo::transport
