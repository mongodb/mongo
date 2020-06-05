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

#include "mongo/platform/basic.h"

#include "mongo/db/cursor_id.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/commands/strategy.h"

namespace mongo {

using InspectionCallback = std::function<void(const executor::RemoteCommandRequest& request)>;

class ClusterCommandTestFixture : public CatalogCacheTestFixture {
protected:
    const size_t numShards = 2;

    const NamespaceString kNss = NamespaceString("test", "coll");

    const LogicalTime kInMemoryLogicalTime = LogicalTime(Timestamp(10, 1));

    static const Timestamp kAfterClusterTime;

    static const Timestamp kShardClusterTime;

    static const CursorId kCursorId;

    void setUp() override;

    void tearDown() override;

    virtual void expectInspectRequest(int shardIndex, InspectionCallback cb) = 0;

    virtual void expectReturnsSuccess(int shardIndex) = 0;

    void expectReturnsError(ErrorCodes::Error code);

    void expectAbortTransaction();

    DbResponse runCommand(BSONObj cmd);

    void runCommandSuccessful(BSONObj cmd, bool isTargeted);

    void runTxnCommandOneError(BSONObj cmd, ErrorCodes::Error code, bool isTargeted);

    void runCommandInspectRequests(BSONObj cmd, InspectionCallback cb, bool isTargeted);

    void runTxnCommandMaxErrors(BSONObj cmd, ErrorCodes::Error code, bool isTargeted);

    /**
     * Verifies that running the given commands through mongos will succeed.
     */
    void testNoErrors(BSONObj targetedCmd, BSONObj scatterGatherCmd = BSONObj());

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
     * Verifies that atClusterTime is attached to the given commands in a transaction.
     */
    void testAttachesAtClusterTimeForTxnSnapshotReadConcern(BSONObj targetedCmd,
                                                            BSONObj scatterGatherCmd = BSONObj(),
                                                            bool createsCursor = false);

    /**
     * Verifies that the chosen atClusterTime is greater than or equal to each command's
     * afterClusterTime in a transaction.
     */
    void testTxnSnapshotReadConcernWithAfterClusterTime(BSONObj targetedCmd,
                                                        BSONObj scatterGatherCmd = BSONObj(),
                                                        bool createsCursor = false);

    /**
     * Verifies that atClusterTime is attached to the given non-transaction snapshot commands.
     */
    void testAttachesAtClusterTimeForNonTxnSnapshotReadConcern(BSONObj targetedCmd,
                                                               BSONObj scatterGatherCmd = BSONObj(),
                                                               bool createsCursor = false);

    /**
     * Verifies that the chosen atClusterTime is greater than or equal to each non-transaction
     * snapshot command's afterClusterTime.
     */
    void testNonTxnSnapshotReadConcernWithAfterClusterTime(BSONObj targetedCmd,
                                                           BSONObj scatterGatherCmd = BSONObj(),
                                                           bool createsCursor = false);

    /**
     * Appends the metadata shards return on responses to transaction statements, such as the
     * readOnly field.
     */
    void appendTxnResponseMetadata(BSONObjBuilder& bob);

private:
    /**
     * Makes a new command object from the one given by appending read concern snapshot and the
     * appropriate transaction options. If includeAfterClusterTime is true, also appends
     * afterClusterTime to the read concern.
     */
    BSONObj _makeTxnCmd(BSONObj cmdObj, bool includeAfterClusterTime = false);

    /**
     * Makes a new command object from the one given by appending read concern snapshot. If
     * includeAfterClusterTime is true, also appends afterClusterTime to the read concern.
     */
    BSONObj _makeNonTxnCmd(BSONObj cmdObj, bool includeAfterClusterTime = false);

    /**
     * Helper method.
     */
    BSONObj _makeCmd(BSONObj cmdObj, bool startTransaction, bool includeAfterClusterTime);

    /*
     * Check that the ClusterCursorManager contains an idle cursor with proper readConcern set.
     */
    void _assertCursorReadConcern(bool isTargeted,
                                  boost::optional<Timestamp> expectedAtClusterTime);

    static void _containsSelectedAtClusterTime(const executor::RemoteCommandRequest& request);

    static void _containsAtClusterTimeOnly(const executor::RemoteCommandRequest& request);

    static void _containsAfterClusterTimeOnly(const executor::RemoteCommandRequest& request);

    static void _omitsClusterTime(const executor::RemoteCommandRequest& request);

    // Enables the transaction router to retry within a transaction on stale version and snapshot
    // errors for the duration of each test.
    // TODO SERVER-39704: Remove this failpoint block.
    std::unique_ptr<FailPointEnableBlock> _staleVersionAndSnapshotRetriesBlock;
};

}  // namespace mongo
