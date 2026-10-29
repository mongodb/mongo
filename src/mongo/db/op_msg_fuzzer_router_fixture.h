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
class OpMsgFuzzerRouterFixture {
public:
    OpMsgFuzzerRouterFixture(bool skipGlobalInitializers = false);

    /**
     * Run a single operation as if it came from the network.
     */
    int testOneInput(const char* Data, size_t Size);

private:
    void _setAuthorizationManager();

    ServiceContext* _serviceContext;
    transport::TransportLayerMock _transportLayer;
    std::shared_ptr<transport::Session> _session;

    ClientStrandPtr _routerStrand;
};
}  // namespace mongo
