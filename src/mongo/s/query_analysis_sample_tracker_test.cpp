/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/s/query_analysis_sample_tracker.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/sharding_test_fixture_common.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <cstddef>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace analyze_shard_key {
namespace {

const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("testDb", "testColl0");

static const std::string kCurrentOpDescFieldValue{"query analyzer"};

class QueryAnalysisSampleTrackerTest : public ShardingTestFixtureCommon {
public:
    /**
     * Run through sample counters refreshConfiguration and increment functions depending on
     * whether process is mongod or mongos.
     */
    void testRefreshConfigIncrementAndReport();

protected:
    Date_t now() {
        return getServiceContext()->getFastClockSource()->now();
    }

    const NamespaceString nss0 =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl0");
    const NamespaceString nss1 =
        NamespaceString::createNamespaceString_forTest("testDb", "testColl1");
    const UUID collUuid0 = UUID::gen();
    const UUID collUuid1 = UUID::gen();
};

void QueryAnalysisSampleTrackerTest::testRefreshConfigIncrementAndReport() {
    const bool supportsSampling =
        isMongos() || serverGlobalParams.clusterRole.has(ClusterRole::None);
    const bool supportsPersisting = !isMongos() &&
        (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer) ||
         serverGlobalParams.clusterRole.has(ClusterRole::None));

    // The mock size for each sampled read or write query, in bytes.
    const int64_t sampledQueryDocSizeBytes = 10;

    auto opCtx = operationContext();
    QueryAnalysisSampleTracker& tracker = QueryAnalysisSampleTracker::get(opCtx);

    // Add the configuration for collection0 and refresh.
    std::vector<CollectionQueryAnalyzerConfiguration> configurationsV1;
    const double sampleRate0Before = 0.0;
    const auto startTime0Before = now();
    configurationsV1.emplace_back(nss0, collUuid0, sampleRate0Before, startTime0Before);
    tracker.refreshConfigurations(configurationsV1);

    // Verify currentOp, one active collection.
    std::vector<BSONObj> ops;
    tracker.reportForCurrentOp(&ops);
    ASSERT_EQ(1, ops.size());
    auto parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[0]);
    ASSERT_EQ(parsedOp.getDesc(), kCurrentOpDescFieldValue);
    ASSERT_EQ(parsedOp.getNs(), nss0);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid0);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate0Before);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime0Before);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 0);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 0);
    if (supportsPersisting) {
        ASSERT_EQ(*(parsedOp.getSampledReadsBytes()), 0L);
        ASSERT_EQ(*(parsedOp.getSampledWritesBytes()), 0L);
    }

    // Verify server status, one active collection.
    BSONObj serverStatus;
    serverStatus = tracker.reportForServerStatus();
    auto parsedServerStatus = QueryAnalysisServerStatus::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        serverStatus);
    ASSERT_EQ(parsedServerStatus.getActiveCollections(), 1);
    ASSERT_EQ(parsedServerStatus.getTotalCollections(), 1);
    ASSERT_EQ(parsedServerStatus.getTotalSampledReadsCount(), 0);
    ASSERT_EQ(parsedServerStatus.getTotalSampledWritesCount(), 0);
    if (supportsPersisting) {
        ASSERT_EQ(*parsedServerStatus.getTotalSampledReadsBytes(), 0L);
        ASSERT_EQ(*parsedServerStatus.getTotalSampledWritesBytes(), 0L);
    }

    // Add the configuration for collection1 and refresh.
    std::vector<CollectionQueryAnalyzerConfiguration> configurationsV2;
    configurationsV2.emplace_back(nss0, collUuid0, sampleRate0Before, startTime0Before);
    const double sampleRate1Before = 0.0000000001;
    const auto startTime1Before = now();
    configurationsV2.emplace_back(nss1, collUuid1, sampleRate1Before, startTime1Before);
    tracker.refreshConfigurations(configurationsV2);

    // Verify currentOp, two active collections.
    ops.clear();
    tracker.reportForCurrentOp(&ops);
    ASSERT_EQ(2, ops.size());
    parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[0]);
    ASSERT_EQ(parsedOp.getDesc(), kCurrentOpDescFieldValue);
    ASSERT_EQ(parsedOp.getNs(), nss0);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid0);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate0Before);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime0Before);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 0);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 0);
    if (supportsPersisting) {
        ASSERT_EQ(*(parsedOp.getSampledReadsBytes()), 0L);
        ASSERT_EQ(*(parsedOp.getSampledWritesBytes()), 0L);
    }
    parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[1]);
    ASSERT_EQ(parsedOp.getNs(), nss1);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid1);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate1Before);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime1Before);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 0);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 0);
    if (supportsPersisting) {
        ASSERT_EQ(*(parsedOp.getSampledReadsBytes()), 0L);
        ASSERT_EQ(*(parsedOp.getSampledWritesBytes()), 0L);
    }

    // Verify server status, two active collections.
    serverStatus = tracker.reportForServerStatus();
    parsedServerStatus = QueryAnalysisServerStatus::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        serverStatus);
    ASSERT_EQ(parsedServerStatus.getActiveCollections(), 2);
    ASSERT_EQ(parsedServerStatus.getTotalCollections(), 2);
    ASSERT_EQ(parsedServerStatus.getTotalSampledReadsCount(), 0);
    ASSERT_EQ(parsedServerStatus.getTotalSampledWritesCount(), 0);
    if (supportsPersisting) {
        ASSERT_EQ(*parsedServerStatus.getTotalSampledReadsBytes(), 0L);
        ASSERT_EQ(*parsedServerStatus.getTotalSampledWritesBytes(), 0L);
    }

    // Update the sample rate for collection1 and refresh.
    std::vector<CollectionQueryAnalyzerConfiguration> configurationsV3;
    configurationsV3.emplace_back(nss0, collUuid0, sampleRate0Before, startTime0Before);
    configurationsV3.emplace_back(nss1, collUuid1, sampleRate1Before, startTime1Before);
    tracker.refreshConfigurations(configurationsV3);

    // Increment read and write counters.
    if (supportsPersisting) {
        tracker.incrementReads(opCtx, nss0, collUuid0, sampledQueryDocSizeBytes);
        tracker.incrementWrites(opCtx, nss0, collUuid0, sampledQueryDocSizeBytes);
        tracker.incrementReads(opCtx, nss1, collUuid1, sampledQueryDocSizeBytes);
        tracker.incrementReads(opCtx, nss1, collUuid1, sampledQueryDocSizeBytes);
        tracker.incrementWrites(opCtx, nss1, collUuid1, sampledQueryDocSizeBytes);
    } else if (supportsSampling) {
        tracker.incrementReads(opCtx, nss0, collUuid0);
        tracker.incrementWrites(opCtx, nss0, collUuid0);
        tracker.incrementReads(opCtx, nss1, collUuid1);
        tracker.incrementReads(opCtx, nss1, collUuid1);
        tracker.incrementWrites(opCtx, nss1, collUuid1);
    }

    // Verify currentOp, two active collections.
    ops.clear();
    tracker.reportForCurrentOp(&ops);
    ASSERT_EQ(2, ops.size());
    parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[0]);
    ASSERT_EQ(parsedOp.getNs(), nss0);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid0);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate0Before);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime0Before);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 1);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 1);
    if (supportsPersisting) {
        ASSERT_EQ(*(parsedOp.getSampledReadsBytes()), sampledQueryDocSizeBytes);
        ASSERT_EQ(*(parsedOp.getSampledWritesBytes()), sampledQueryDocSizeBytes);
    }
    parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[1]);
    ASSERT_EQ(parsedOp.getNs(), nss1);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid1);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate1Before);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime1Before);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 2);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 1);
    if (supportsPersisting) {
        ASSERT_EQ(*(parsedOp.getSampledReadsBytes()), 2 * sampledQueryDocSizeBytes);
        ASSERT_EQ(*(parsedOp.getSampledWritesBytes()), sampledQueryDocSizeBytes);
    }

    // Verify server status, two active collections.
    serverStatus = tracker.reportForServerStatus();
    parsedServerStatus = QueryAnalysisServerStatus::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        serverStatus);
    ASSERT_EQ(parsedServerStatus.getActiveCollections(), 2);
    ASSERT_EQ(parsedServerStatus.getTotalCollections(), 2);
    ASSERT_EQ(parsedServerStatus.getTotalSampledReadsCount(), 3);
    ASSERT_EQ(parsedServerStatus.getTotalSampledWritesCount(), 2);
    if (supportsPersisting) {
        ASSERT_EQ(*parsedServerStatus.getTotalSampledReadsBytes(), 3 * sampledQueryDocSizeBytes);
        ASSERT_EQ(*parsedServerStatus.getTotalSampledWritesBytes(), 2 * sampledQueryDocSizeBytes);
    }

    // Remove the configuration for collection1.
    std::vector<CollectionQueryAnalyzerConfiguration> configurationsV4;
    configurationsV4.emplace_back(nss0, collUuid0, sampleRate0Before, startTime0Before);
    tracker.refreshConfigurations(configurationsV4);

    // Verify currentOp, one active configuration.
    ops.clear();
    tracker.reportForCurrentOp(&ops);
    ASSERT_EQ(1, ops.size());
    parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[0]);
    ASSERT_EQ(parsedOp.getNs(), nss0);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid0);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate0Before);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime0Before);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 1);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 1);
    if (supportsPersisting) {
        ASSERT_EQ(*(parsedOp.getSampledReadsBytes()), sampledQueryDocSizeBytes);
        ASSERT_EQ(*(parsedOp.getSampledWritesBytes()), sampledQueryDocSizeBytes);
    }

    // Verify server status, one active configuration.
    serverStatus = tracker.reportForServerStatus();
    parsedServerStatus = QueryAnalysisServerStatus::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        serverStatus);
    ASSERT_EQ(parsedServerStatus.getActiveCollections(), 1);
    ASSERT_EQ(parsedServerStatus.getTotalCollections(), 2);
    ASSERT_EQ(parsedServerStatus.getTotalSampledReadsCount(), 3);
    ASSERT_EQ(parsedServerStatus.getTotalSampledWritesCount(), 2);
    if (supportsPersisting) {
        ASSERT_EQ(*parsedServerStatus.getTotalSampledReadsBytes(), 3 * sampledQueryDocSizeBytes);
        ASSERT_EQ(*parsedServerStatus.getTotalSampledWritesBytes(), 2 * sampledQueryDocSizeBytes);
    }

    // Add new configuration for collection1 (same collection uuid).
    std::vector<CollectionQueryAnalyzerConfiguration> configurationsV5;
    configurationsV5.emplace_back(nss0, collUuid0, sampleRate0Before, startTime0Before);
    const double sampleRate1After = 222.2;
    const auto startTime1After = now();
    configurationsV5.emplace_back(nss1, collUuid1, sampleRate1After, startTime1After);
    tracker.refreshConfigurations(configurationsV5);

    // Verify currentOp, two active collections.
    ops.clear();
    tracker.reportForCurrentOp(&ops);
    ASSERT_EQ(2, ops.size());
    parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[0]);
    ASSERT_EQ(parsedOp.getDesc(), kCurrentOpDescFieldValue);
    ASSERT_EQ(parsedOp.getNs(), nss0);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid0);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate0Before);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime0Before);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 1);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 1);
    if (supportsPersisting) {
        ASSERT_EQ(parsedOp.getSampledReadsBytes(), sampledQueryDocSizeBytes);
        ASSERT_EQ(parsedOp.getSampledWritesBytes(), sampledQueryDocSizeBytes);
    }
    parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[1]);
    ASSERT_EQ(parsedOp.getDesc(), kCurrentOpDescFieldValue);
    ASSERT_EQ(parsedOp.getNs(), nss1);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid1);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate1After);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime1After);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 0);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 0);
    if (supportsPersisting) {
        ASSERT_EQ(*(parsedOp.getSampledReadsBytes()), 0L);
        ASSERT_EQ(*(parsedOp.getSampledWritesBytes()), 0L);
    }

    // Verify server status, two active collections.
    serverStatus = tracker.reportForServerStatus();
    parsedServerStatus = QueryAnalysisServerStatus::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        serverStatus);
    ASSERT_EQ(parsedServerStatus.getActiveCollections(), 2);
    ASSERT_EQ(parsedServerStatus.getTotalCollections(), 2);
    ASSERT_EQ(parsedServerStatus.getTotalSampledReadsCount(), 3);
    ASSERT_EQ(parsedServerStatus.getTotalSampledWritesCount(), 2);
    if (supportsPersisting) {
        ASSERT_EQ(*parsedServerStatus.getTotalSampledReadsBytes(), 3 * sampledQueryDocSizeBytes);
        ASSERT_EQ(*parsedServerStatus.getTotalSampledWritesBytes(), 2 * sampledQueryDocSizeBytes);
    }

    // Remove the configuration for collection1, and make the configuration for collection0 have
    // a different collection uuid.
    std::vector<CollectionQueryAnalyzerConfiguration> configurationsV6;
    const auto collUuid0After = UUID::gen();
    const auto sampleRate0After = 1.5;
    const auto startTime0After = now();
    configurationsV6.emplace_back(nss0, collUuid0After, sampleRate0After, startTime0After);
    tracker.refreshConfigurations(configurationsV6);

    // Verify currentOp, one active collection. The counters should have been reset.
    ops.clear();
    tracker.reportForCurrentOp(&ops);
    ASSERT_EQ(1, ops.size());
    parsedOp = CollectionSampleCountersCurrentOp::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        ops[0]);
    ASSERT_EQ(parsedOp.getDesc(), kCurrentOpDescFieldValue);
    ASSERT_EQ(parsedOp.getNs(), nss0);
    ASSERT_EQ(parsedOp.getCollUuid(), collUuid0After);
    if (supportsSampling) {
        ASSERT_EQ(parsedOp.getSampleRate(), sampleRate0After);
    }
    ASSERT_EQ(parsedOp.getStartTime(), startTime0After);
    ASSERT_EQ(parsedOp.getSampledReadsCount(), 0);
    ASSERT_EQ(parsedOp.getSampledWritesCount(), 0);
    if (supportsPersisting) {
        ASSERT_EQ(*parsedOp.getSampledReadsBytes(), 0);
        ASSERT_EQ(*parsedOp.getSampledWritesBytes(), 0);
    }

    // Verify server status, one active collection.
    serverStatus = tracker.reportForServerStatus();
    parsedServerStatus = QueryAnalysisServerStatus::parse(
        IDLParserContext("QueryAnalysisSampleTrackerTest.RefreshConfigIncrementAndReport_TEST"),
        serverStatus);
    ASSERT_EQ(parsedServerStatus.getActiveCollections(), 1);
    ASSERT_EQ(parsedServerStatus.getTotalCollections(), 2);
    ASSERT_EQ(parsedServerStatus.getTotalSampledReadsCount(), 3);
    ASSERT_EQ(parsedServerStatus.getTotalSampledWritesCount(), 2);
    if (supportsPersisting) {
        ASSERT_EQ(*parsedServerStatus.getTotalSampledReadsBytes(), 3 * sampledQueryDocSizeBytes);
        ASSERT_EQ(*parsedServerStatus.getTotalSampledWritesBytes(), 2 * sampledQueryDocSizeBytes);
    }
}

TEST_F(QueryAnalysisSampleTrackerTest, RefreshConfigIncrementAndReportMongos) {
    bool originalIsMongos = isMongos();
    ON_BLOCK_EXIT([&] { setMongos(originalIsMongos); });

    setMongos(true);
    testRefreshConfigIncrementAndReport();
}

TEST_F(QueryAnalysisSampleTrackerTest, RefreshConfigIncrementAndReportShardSvrMongod) {
    bool originalIsMongos = isMongos();
    auto originalRole = serverGlobalParams.clusterRole;
    ON_BLOCK_EXIT([&] {
        setMongos(originalIsMongos);
        serverGlobalParams.clusterRole = originalRole;
    });

    setMongos(false);
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    testRefreshConfigIncrementAndReport();
}

TEST_F(QueryAnalysisSampleTrackerTest, RefreshConfigIncrementAndReportReplSetMongod) {
    bool originalIsMongos = isMongos();
    auto originalRole = serverGlobalParams.clusterRole;
    ON_BLOCK_EXIT([&] {
        setMongos(originalIsMongos);
        serverGlobalParams.clusterRole = originalRole;
    });

    setMongos(false);
    serverGlobalParams.clusterRole = ClusterRole::None;
    testRefreshConfigIncrementAndReport();
}

}  // namespace
}  // namespace analyze_shard_key
}  // namespace mongo
