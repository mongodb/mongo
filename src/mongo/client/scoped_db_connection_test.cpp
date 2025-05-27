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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/dbtests/mock/mock_conn_registry.h"
#include "mongo/dbtests/mock/mock_remote_db_server.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace mongo {
namespace {
class ConnectionPoolTest : public unittest::Test {
public:
    void setUp() override {
        auto& settings = logv2::LogManager::global().getGlobalSettings();
        _originalSeverity = settings.getMinimumLogSeverity(logv2::LogComponent::kNetwork).toInt();
        settings.setMinimumLoggedSeverity(logv2::LogComponent::kNetwork,
                                          logv2::LogSeverity::Debug(1));

        ConnectionString::setConnectionHook(MockConnRegistry::get()->getConnStrHook());
        _mockServer = std::make_unique<MockRemoteDBServer>(_hostName);
        MockConnRegistry::get()->addServer(_mockServer.get());
    }

    void tearDown() override {
        MockConnRegistry::get()->removeServer(_hostName);

        auto& settings = logv2::LogManager::global().getGlobalSettings();
        settings.setMinimumLoggedSeverity(logv2::LogComponent::kNetwork,
                                          logv2::LogSeverity::cast(_originalSeverity));
    }

    auto getServerHostAndPort() const {
        return _mockServer->getServerHostAndPort();
    }

private:
    std::string _hostName = "$local";
    std::unique_ptr<MockRemoteDBServer> _mockServer;
    int _originalSeverity;
};

TEST_F(ConnectionPoolTest, ConnectionPoolHistogramStats) {
    using namespace unittest::match;

    FailPointEnableBlock fp("injectWaitTimeForConnpoolAcquisition", BSON("sleepTimeMillis" << 60));

    const auto host = getServerHostAndPort().toString();
    executor::ConnectionPoolStats stats{};

    ScopedDbConnection conn(host);
    auto connTime = globalConnPool.getPoolHostConnTime_forTest(host, 0).count();

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

    const auto pos = [&]() -> size_t {
        using namespace executor::details;
        if (connTime >= kMaxPartitionSize) {
            return histogram.size() - 1;
        } else if (connTime < kStartSize) {
            return 0;
        }
        return ((connTime - kStartSize) / kPartitionStepSize) + 1;
    }();
    ASSERT_EQ(histogram, makeExpected(pos));
}
}  // namespace
}  // namespace mongo
