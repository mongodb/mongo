// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
