// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/rpc/metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

#include <memory>

namespace mongo {
namespace executor {

class NetworkInterfaceMockTest : public ServiceContextTest {
public:
    NetworkInterfaceMockTest()
        : _net{}, _executor(&_net, 1, ThreadPoolMock::Options()), _tearDownCalled(false) {}

    NetworkInterfaceMock& net() {
        return _net;
    }

    ThreadPoolMock& executor() {
        return _executor;
    }

    HostAndPort testHost() {
        return {"localHost", 27017};
    }

    void startNetwork();
    void setUp() override;
    void tearDown() override;

    RemoteCommandRequest kUnimportantRequest{
        testHost(),
        DatabaseName::createDatabaseName_forTest(boost::none, "testDB"),
        BSON("test" << 1),
        rpc::makeEmptyMetadata(),
        nullptr};

private:
    NetworkInterfaceMock _net;
    ThreadPoolMock _executor;
    bool _tearDownCalled;
};

}  // namespace executor
}  // namespace mongo
