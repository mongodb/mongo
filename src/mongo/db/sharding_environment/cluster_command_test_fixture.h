// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_test_fixture.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/stats/counters.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/tick_source_mock.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace mongo {

using InspectionCallback = std::function<void(const executor::RemoteCommandRequest& request)>;

class ClusterCommandTestFixture : public RouterCatalogCacheTestFixture {
protected:
    const size_t numShards = 2;

    const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test", "coll");

    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(10, 1));

    const Timestamp kAfterClusterTime = Timestamp(50, 2);

    ClusterCommandTestFixture()
        : RouterCatalogCacheTestFixture(std::make_unique<ScopedGlobalServiceContextForTest>(
              ServiceContext::make(std::make_unique<ClockSourceMock>(),
                                   std::make_unique<ClockSourceMock>(),
                                   std::make_unique<TickSourceMock<Microseconds>>()))) {}

    void setUp() override;

    virtual void expectInspectRequest(int shardIndex, InspectionCallback cb) = 0;

    virtual void expectReturnsSuccess(int shardIndex) = 0;

    void expectReturnsError(ErrorCodes::Error code);

    void expectAbortTransaction();

    OpMsgRequest makeRequest(const NamespaceString& nss, const BSONObj& body);

    DbResponse runCommand(BSONObj cmd);

    DbResponse runCommandSuccessful(BSONObj cmd, bool isTargeted);

    void runTxnCommandOneError(BSONObj cmd, ErrorCodes::Error code, bool isTargeted);

    void runCommandInspectRequests(BSONObj cmd, InspectionCallback cb, bool isTargeted);

    void runTxnCommandMaxErrors(BSONObj cmd, ErrorCodes::Error code, bool isTargeted);

    /**
     * Verifies that running the given commands through mongos will succeed.
     */
    std::vector<DbResponse> testNoErrors(BSONObj targetedCmd, BSONObj scatterGatherCmd = BSONObj());

    std::vector<DbResponse> testNoErrorsOutsideTransaction(BSONObj targetedCmd,
                                                           BSONObj scatterGatherCmd = BSONObj());

    /**
     * Verifies that the given commands will retry on a snapshot error.
     */
    void testRetryOnSnapshotError(BSONObj targetedCmd, BSONObj scatterGatherCmd = BSONObj());

    /**
     * Verifies that the given commands will retry up to the max retry attempts on snapshot
     * errors then return the final errors they receive.
     */
    void testMaxRetriesSnapshotErrors(BSONObj targetedCmd, BSONObj scatterGatherCmd = BSONObj());

    /**
     * Verifies that atClusterTime is attached to the given commands.
     */
    void testAttachesAtClusterTimeForSnapshotReadConcern(BSONObj targetedCmd,
                                                         BSONObj scatterGatherCmd = BSONObj());

    /**
     * Verifies that the chosen atClusterTime is greater than or equal to each command's
     * afterClusterTime.
     */
    void testSnapshotReadConcernWithAfterClusterTime(BSONObj targetedCmd,
                                                     BSONObj scatterGatherCmd = BSONObj());

    /**
     * Verifies that includeQueryStatsMetrics is added or not added as needed.
     */
    void testIncludeQueryStatsMetrics(BSONObj cmd, bool isTargeted);

    /**
     * Verifies that the opcounters change by the given delta values after a command is run.
     */
    void testOpcountersAreCorrect(BSONObj cmd, BSONObj expectedMetricDeltas);

    /**
     * Appends the metadata shards return on responses to transaction statements, such as the
     * readOnly field.
     */
    void appendTxnResponseMetadata(BSONObjBuilder& bob);

private:
    /**
     * Makes a new command object from the one given by apppending read concern
     * snapshot and the appropriate transaction options. If includeAfterClusterTime
     * is true, also appends afterClusterTime to the read concern.
     */
    BSONObj _makeCmd(BSONObj cmdObj, bool includeAfterClusterTime = false);

    // Enables the transaction router to retry within a transaction on stale version and snapshot
    // errors for the duration of each test.
    // TODO SERVER-39704: Remove this failpoint block.
    std::unique_ptr<FailPointEnableBlock> _staleVersionAndSnapshotRetriesBlock;

    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

}  // namespace mongo
