/**
 * A node that completes logical initial sync from a sync source whose oplog has not truncated yet
 * should retain the replica set's 'initiating set' oplog entry as the first entry of its own oplog.
 *
 * Change streams rely on that entry to prove that no history has fallen off a node's oplog (see
 * initCursorAndAssertTsHasNotFallenOff() in collection_scan.cpp). If initial sync does not fetch the
 * entry from the sync source, the syncing node's oplog begins after it, and a change stream that
 * starts from before that point fails with ChangeStreamHistoryLost on the syncing node, even though
 * its sync source could have served it.
 *
 * @tags: [requires_replication, multiversion_incompatible]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("initial sync and the 'initiating set' oplog entry", function () {
    const dbName = "test";
    const collName = "resumability";
    const kInitiatingSetMsg = "initiating set";

    // Log ids emitted by InitialSyncer::_handleOldestOplogEntryForInitiatingSetResponse().
    const kRewindLogId = 12195600;
    const kRewindSkippedTooOldLogId = 12195601;
    const kRewindDisabledAfterFailureLogId = 12195603;

    function firstOplogEntry(node) {
        return node.getDB("local").oplog.rs.find().sort({$natural: 1}).limit(1).toArray()[0];
    }

    // The repl.initialSync.initiatingSetRewind counters on 'node', with absent counters as 0.
    function rewindMetrics(node) {
        const metrics =
            assert.commandWorked(node.adminCommand({serverStatus: 1})).metrics.repl.initialSync
                .initiatingSetRewind ?? {};
        return {
            rewound: metrics.rewound ?? 0,
            skippedTooOld: metrics.skippedTooOld ?? 0,
            oldestEntryParseFailures: metrics.oldestEntryParseFailures ?? 0,
            disabledAfterFailedAttempt: metrics.disabledAfterFailedAttempt ?? 0,
        };
    }

    function isInitiatingSetEntry(entry) {
        return entry.op === "n" && entry.o !== undefined && entry.o.msg === kInitiatingSetMsg;
    }

    /**
     * Initiates a one-node replica set, writes past its 'initiating set' entry, then adds a
     * priority 0, non-voting node and lets it initial sync. Returns what the test needs to know
     * about the new node.
     */
    function addNodeToNewReplicaSet({
        setParameter,
        changeFcv = false,
        delaySecs = 0,
        failFirstBatchOnce = false,
    }) {
        const rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter}});
        rst.startSet();
        rst.initiate();

        const primary = rst.getPrimary();
        const primaryDB = primary.getDB(dbName);

        // The replica set has just been initiated, so the primary's oplog starts at the marker.
        const markerEntry = firstOplogEntry(primary);
        assert(isInitiatingSetEntry(markerEntry), "primary's oplog should start at the marker", {
            markerEntry,
        });

        // Advance the primary's oplog past the marker. Without these writes the marker would still
        // be the top of the oplog when the new node picks its fetch start point.
        assert.commandWorked(
            primaryDB[collName].insert([{_id: 0}, {_id: 1}, {_id: 2}], {writeConcern: {w: 1}}),
        );

        if (changeFcv) {
            // Put an FCV transition between the marker and where the new node begins applying.
            // This is what addShard does: it runs setFCV against the new shard after
            // replSetInitiate.
            assert.commandWorked(
                primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
            );
            assert.commandWorked(
                primary.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            );
            assert.commandWorked(primaryDB[collName].insert({_id: 3}, {writeConcern: {w: 1}}));
        }

        if (delaySecs > 0) {
            sleep(delaySecs * 1000);
            assert.commandWorked(primaryDB[collName].insert({_id: 4}, {writeConcern: {w: 1}}));
        }

        const newNode = rst.add({rsConfig: {priority: 0, votes: 0}, setParameter});
        if (failFirstBatchOnce) {
            // Fail the first attempt right before it writes its first batch, which only holds
            // entries at or before the beginApplyingTimestamp.
            configureFailPoint(newNode, "failInitialSyncBeforeApplyingBatch", {}, {times: 1});
        }
        const hangBeforeFinish = configureFailPoint(newNode, "initialSyncHangBeforeFinish");
        rst.reInitiate();
        hangBeforeFinish.wait();
        const statusRes = assert.commandWorked(newNode.adminCommand({replSetGetStatus: 1}));
        const initialSyncStatus = statusRes.initialSyncStatus;
        hangBeforeFinish.off();

        rst.awaitSecondaryNodes(null, [newNode]);
        rst.awaitReplication();

        const newNodeFirstEntry = firstOplogEntry(newNode);
        jsTest.log.info("New node finished initial sync", {initialSyncStatus, newNodeFirstEntry});

        // A resume point from before the replica set existed, as for a change stream that was
        // already running when the shard that is now being asked to serve it was added.
        newNode.setSecondaryOk();
        const changeStreamRes = newNode.getDB(dbName).runCommand({
            aggregate: collName,
            pipeline: [
                {
                    $changeStream: {
                        startAtOperationTime: Timestamp(markerEntry.ts.getTime() - 60, 1),
                    },
                },
            ],
            cursor: {},
        });

        return {
            rst,
            newNode,
            markerEntry,
            newNodeFirstEntry,
            initialSyncStatus,
            changeStreamRes,
            metrics: rewindMetrics(newNode),
        };
    }

    function assertRewound({newNode, markerEntry, newNodeFirstEntry, initialSyncStatus, metrics}) {
        assert.docEq(
            {
                rewound: 1,
                skippedTooOld: 0,
                oldestEntryParseFailures: 0,
                disabledAfterFailedAttempt: 0,
            },
            metrics,
        );
        assert(
            isInitiatingSetEntry(newNodeFirstEntry),
            "new node's oplog should start at the marker",
            {
                newNodeFirstEntry,
            },
        );
        assert.eq(markerEntry.ts, newNodeFirstEntry.ts);
        assert(checkLog.checkContainsOnceJson(newNode, kRewindLogId, {}));
        assert.eq(
            markerEntry.ts,
            initialSyncStatus.initialSyncOplogFetchingStart,
            "initial sync should have started fetching at the marker",
            {initialSyncStatus},
        );
        // writtenNotAppliedOps should match exactly the oplog entries from the marker (where
        // fetching started) through the beginApplyingTimestamp (where applying starts), both
        // inclusive: the sync source's own oplog entries in that range plus the local 3 inserts.
        const expectedWrittenNotAppliedOps = newNode.getDB("local").oplog.rs.countDocuments({
            ts: {$gte: markerEntry.ts, $lte: initialSyncStatus.initialSyncOplogStart},
        });
        assert.eq(
            expectedWrittenNotAppliedOps,
            initialSyncStatus.writtenNotAppliedOps,
            "expected writtenNotAppliedOps to match the oplog entries written without being " +
                "applied",
            {initialSyncStatus, expectedWrittenNotAppliedOps},
        );
    }

    it("fails to resume a change stream on the new node without the rewind", function () {
        const res = addNodeToNewReplicaSet({
            setParameter: {initialSyncFetchFromInitiatingSetEntry: false},
        });
        assert(
            !isInitiatingSetEntry(res.newNodeFirstEntry),
            "new node's oplog should not start at the marker",
            {
                newNodeFirstEntry: res.newNodeFirstEntry,
            },
        );
        assert.commandFailedWithCode(res.changeStreamRes, ErrorCodes.ChangeStreamHistoryLost);
        assert.docEq(
            {
                rewound: 0,
                skippedTooOld: 0,
                oldestEntryParseFailures: 0,
                disabledAfterFailedAttempt: 0,
            },
            res.metrics,
        );
        res.rst.stopSet();
    });

    it("keeps the marker on the new node so the change stream can resume", function () {
        const res = addNodeToNewReplicaSet({
            setParameter: {initialSyncFetchFromInitiatingSetEntry: true},
        });
        assertRewound(res);
        assert.commandWorked(res.changeStreamRes);
        res.rst.stopSet();
    });

    it("writes an FCV change in the widened fetch range without applying it", function () {
        const res = addNodeToNewReplicaSet({
            setParameter: {initialSyncFetchFromInitiatingSetEntry: true},
            changeFcv: true,
        });
        assertRewound(res);
        assert.commandWorked(res.changeStreamRes);

        // The FCV entries must be in the new node's oplog. Had they been applied, initial sync would
        // have failed.
        const fcvEntries = res.newNode
            .getDB("local")
            .oplog.rs.find({ns: "admin.system.version", ts: {$gt: res.markerEntry.ts}})
            .itcount();
        assert.gt(fcvEntries, 0, "expected the FCV oplog entries in the new node's oplog");
        res.rst.stopSet();
    });

    it("stops rewinding after an attempt fails while writing the widened range", function () {
        const res = addNodeToNewReplicaSet({
            setParameter: {initialSyncFetchFromInitiatingSetEntry: true, numInitialSyncAttempts: 2},
            failFirstBatchOnce: true,
        });
        assert.eq(
            1,
            res.initialSyncStatus.failedInitialSyncAttempts,
            "expected one failed attempt",
            {
                initialSyncStatus: res.initialSyncStatus,
            },
        );
        assert(checkLog.checkContainsOnceJson(res.newNode, kRewindDisabledAfterFailureLogId, {}));
        assert.docEq(
            {
                rewound: 1,
                skippedTooOld: 0,
                oldestEntryParseFailures: 0,
                disabledAfterFailedAttempt: 1,
            },
            res.metrics,
        );
        assert.eq(1, checkLog.getFilteredLogMessages(res.newNode, kRewindLogId, {}).length);
        assert(
            !isInitiatingSetEntry(res.newNodeFirstEntry),
            "new node's oplog should not start at the marker",
            {
                newNodeFirstEntry: res.newNodeFirstEntry,
            },
        );
        assert.commandFailedWithCode(res.changeStreamRes, ErrorCodes.ChangeStreamHistoryLost);
        res.rst.stopSet();
    });

    it("does not rewind when the marker is older than the configured bound", function () {
        const res = addNodeToNewReplicaSet({
            setParameter: {
                initialSyncFetchFromInitiatingSetEntry: true,
                initialSyncFetchFromInitiatingSetEntryMaxSecs: 1,
            },
            delaySecs: 3,
        });
        assert(
            !isInitiatingSetEntry(res.newNodeFirstEntry),
            "new node's oplog should not start at the marker",
            {
                newNodeFirstEntry: res.newNodeFirstEntry,
            },
        );
        assert(checkLog.checkContainsOnceJson(res.newNode, kRewindSkippedTooOldLogId, {}));
        assert.docEq(
            {
                rewound: 0,
                skippedTooOld: 1,
                oldestEntryParseFailures: 0,
                disabledAfterFailedAttempt: 0,
            },
            res.metrics,
        );
        assert.commandFailedWithCode(res.changeStreamRes, ErrorCodes.ChangeStreamHistoryLost);
        res.rst.stopSet();
    });
});
