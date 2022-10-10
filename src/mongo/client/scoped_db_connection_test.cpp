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

#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class ConnectionPoolTest : public unittest::Test {
public:
    void setUp() override {
        ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
        _mockServer = std::make_unique<MockRemoteDBServer>(_hostName);
        MockConnRegistry::get()->addServer(_mockServer.get());
    }

    void tearDown() override {
        MockConnRegistry::get()->removeServer(_hostName);
    }

    auto getServerHostAndPort() const {
        return _mockServer->getServerHostAndPort();
    }

private:
    std::string _hostName = "$local";
    std::unique_ptr<MockRemoteDBServer> _mockServer;
};

TEST_F(ConnectionPoolTest, ConnectionPoolHistogramStats) {
    using namespace unittest::match;

    RAIIServerParameterControllerForTest controller("featureFlagConnHealthMetrics", true);
    FailPointEnableBlock fp("injectWaitTimeForConnpoolAcquisition", BSON("sleepTimeMillis" << 60));

    const auto host = getServerHostAndPort().toString();
    executor::ConnectionPoolStats stats{};

    ScopedDbConnection conn(host);
    ASSERT_TRUE(conn.ok());
    globalConnPool.appendConnectionStats(&stats);

    auto histogram = stats.acquisitionWaitTimes.getCounts();

    // Make a container of similar size and type to histogram where the elements at offset `pos` is
    // one.
    auto makeExpected = [&](size_t pos) {
        decltype(histogram) expected(histogram.size(), 0);
        expected[pos] = 1;

        return expected;
    };
    ASSERT_THAT(histogram, AnyOf(Eq(makeExpected(2)), Eq(makeExpected(3))));
}
}  // namespace
}  // namespace mongo
