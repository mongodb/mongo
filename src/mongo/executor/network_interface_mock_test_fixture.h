/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/string_data.h"
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
