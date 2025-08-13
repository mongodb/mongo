/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
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
     * Verifies that the opcounters match the expected values after a command is run.
     */
    void testOpcountersAreCorrect(BSONObj cmd, BSONObj expectedMetrics);

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
