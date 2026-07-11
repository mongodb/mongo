// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/authorization_manager_impl.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/temp_dir.h"

#include <cstddef>
#include <memory>

namespace mongo {
/**
 * This is a simple fixture for use with the OpMsgFuzzer.
 *
 * In essenence, this is equivalent to making a standalone mongod with a single client.
 */
class OpMsgFuzzerShardFixture {
public:
    OpMsgFuzzerShardFixture(bool skipGlobalInitializers = false);

    ~OpMsgFuzzerShardFixture();

    /**
     * Run a single operation as if it came from the network.
     */
    int testOneInput(const char* Data, size_t Size);

private:
    void _setAuthorizationManager();

    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(3, 1));

    // This member is responsible for both creating and deleting the base directory. Think of it as
    // a smart pointer to the directory.
    const unittest::TempDir _dir;

    ServiceContext* _serviceContext;
    transport::TransportLayerMock _transportLayer;
    std::shared_ptr<transport::Session> _session;

    ClientStrandPtr _shardStrand;
};
}  // namespace mongo
