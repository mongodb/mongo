/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#include "mongo/executor/mock_remote_command_runner.h"

#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_runner.h"
#include "mongo/executor/remote_command_runner_test_fixture.h"
#include "mongo/executor/remote_command_targeter.h"
#include "mongo/unittest/unittest.h"

namespace mongo::executor::remote_command_runner {
namespace {

/**
 * This test fixture is used to test the functionality of the mock, rather than test any facilities
 * or usage of the RemoteCommandRunner implementation.
 */
class MockRemoteCommandRunnerTestFixture : public RemoteCommandRunnerTestFixture {
public:
    void setUp() override {
        RemoteCommandRunnerTestFixture::setUp();
        auto uniqueMock = std::make_unique<MockRemoteCommandRunner>();
        _mock = uniqueMock.get();
        detail::RemoteCommandRunner::set(getServiceContext(), std::move(uniqueMock));
    }

    void tearDown() override {
        detail::RemoteCommandRunner::set(getServiceContext(), nullptr);
        RemoteCommandRunnerTestFixture::tearDown();
    }

    MockRemoteCommandRunner& getMockRunner() {
        return *_mock;
    }

private:
    MockRemoteCommandRunner* _mock;
};

// A simple test showing that an arbitrary mock result can be set for a command scheduled through
// the RemoteCommandRunner.
TEST_F(MockRemoteCommandRunnerTestFixture, Example) {
    HelloCommand hello;
    initializeCommand(hello);
    // Doc that shouldn't be parseable as a HelloCommandReply.
    auto invalidResult = BSON("An"
                              << "arbitrary"
                              << "bogus"
                              << "document");
    getMockRunner().setMockResult(invalidResult);

    auto opCtxHolder = makeOperationContext();
    auto res = doRequest(hello,
                         opCtxHolder.get(),
                         std::make_unique<RemoteCommandLocalHostTargeter>(),
                         getExecutorPtr(),
                         _cancellationToken);

    auto check = [&](const DBException& ex) {
        ASSERT_EQ(ex.code(), 40415) << ex.toString();
        ASSERT_STRING_CONTAINS(ex.reason(), "is an unknown field");
    };
    // Ensure we fail to parse the reply due to the unknown fields.
    ASSERT_THROWS_WITH_CHECK(res.get(), DBException, check);
}

}  // namespace
}  // namespace mongo::executor::remote_command_runner
