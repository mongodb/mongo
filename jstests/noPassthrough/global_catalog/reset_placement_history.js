/**
 * Test validating the expected behavior of resetPlacementHistory.
 * @tags: [
 *   featureFlagChangeStreamPreciseShardTargeting,
 *  ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: {rs0: {nodes: 1}, rs1: {nodes: 1}},
    config: {nodes: 1},
});

const configPrimary = st.configRS.getPrimary();

// Identifier for the config.placementHistory special documents holding metadata about the operational boundaries set by the initialization.
const initializationMetadataNssId = "";

// Sends resetPlacementHistory through a parallel shell and pauses it after picking the cluster time for snapshotting the global catalog,
// but before actually performing the read (and materializing the view).
// Returns a handle to unpause and join the resetPlacementHistory issued in
function launchAndPauseResetPlacementHistory() {
    let failPointPausingReset = configureFailPoint(
        configPrimary,
        "initializePlacementHistoryHangAfterSettingSnapshotReadConcern",
        {mode: "alwaysOn"},
    );
    let joinParallelResetRequest = startParallelShell(function () {
        assert.commandWorked(db.getSiblingDB("admin").runCommand({resetPlacementHistory: 1}));
    }, st.s.port);
    failPointPausingReset.wait();

    return {
        failPointPausingReset,
        joinParallelResetRequest,
        resumeAndJoin: function () {
            failPointPausingReset.off();
            joinParallelResetRequest();
        },
    };
}

{
    jsTest.log.info("resetPlacementHistory produces the expected oplog entries across the shards of the cluster");

    const initializationMetadataBeforeReset = st.config.placementHistory
        .find({nss: initializationMetadataNssId})
        .sort({timestamp: 1})
        .toArray();

    assert.eq(initializationMetadataBeforeReset.length, 2);
    assert(
        timestampCmp(initializationMetadataBeforeReset[0].timestamp, initializationMetadataBeforeReset[1].timestamp) !==
            0,
    );
    const initializationTimeBeforeReset = initializationMetadataBeforeReset[1].timestamp;

    assert.commandWorked(st.s.adminCommand({resetPlacementHistory: 1}));

    const initializationMetadataAfterReset = st.config.placementHistory
        .find({nss: initializationMetadataNssId})
        .sort({timestamp: 1})
        .toArray();

    assert.eq(initializationMetadataAfterReset.length, 2);
    assert(
        timestampCmp(initializationMetadataAfterReset[0].timestamp, initializationMetadataAfterReset[1].timestamp) !==
            0,
    );
    const initializationTimeAfterReset = initializationMetadataAfterReset[1].timestamp;

    assert(timestampCmp(initializationTimeAfterReset, initializationTimeBeforeReset) > 0);

    [st.rs0, st.rs1].forEach((rs) => {
        const primary = rs.getPrimary();
        const placementHistoryChangedNotifications = primary
            .getCollection("local.oplog.rs")
            .find({"ns": "", "o2.namespacePlacementChanged": 1})
            .toArray();
        assert.eq(placementHistoryChangedNotifications.length, 1);
        const entry = placementHistoryChangedNotifications[0];
        assert.eq(entry.op, "n");
        assert(timestampCmp(entry.ts, initializationTimeAfterReset) > 0);
    });
}

{
    jsTest.log.info("resetPlacementHistory produces the expected oplog entries across the shards of the cluster");

    const initializationMetadataBeforeReset = st.config.placementHistory
        .find({nss: initializationMetadataNssId})
        .sort({timestamp: 1})
        .toArray();

    assert.eq(initializationMetadataBeforeReset.length, 2);
    assert(
        timestampCmp(initializationMetadataBeforeReset[0].timestamp, initializationMetadataBeforeReset[1].timestamp) !==
            0,
    );
    const initializationTimeBeforeReset = initializationMetadataBeforeReset[1].timestamp;

    assert.commandWorked(st.s.adminCommand({resetPlacementHistory: 1}));

    const initializationMetadataAfterReset = st.config.placementHistory
        .find({nss: initializationMetadataNssId})
        .sort({timestamp: 1})
        .toArray();

    assert.eq(initializationMetadataAfterReset.length, 2);
    assert(
        timestampCmp(initializationMetadataAfterReset[0].timestamp, initializationMetadataAfterReset[1].timestamp) !==
            0,
    );
    const initializationTimeAfterReset = initializationMetadataAfterReset[1].timestamp;

    assert(timestampCmp(initializationTimeAfterReset, initializationTimeBeforeReset) > 0);

    [st.rs0, st.rs1].forEach((rs) => {
        const primary = rs.getPrimary();
        const placementHistoryChangedNotifications = primary
            .getCollection("local.oplog.rs")
            .find({"ns": "", "o2.namespacePlacementChanged": 1})
            .sort({ts: -1})
            .toArray();

        const entry = placementHistoryChangedNotifications[0];
        assert.eq(entry.op, "n");
        assert.eq(entry.o, {msg: {namespacePlacementChanged: ""}});
        assert(timestampCmp(entry.ts, initializationTimeAfterReset) > 0);
    });
}

{
    jsTest.log.info(
        "resetPlacementHistory produces a materialized view that is consistent with the state of the global catalog at the chosen point-in-time",
    );
    const dbName = "consistentExecutionTestDB";
    const nssShardedBeforeReset = `${dbName}.collShardedBeforeReset`;
    const nssShardedAfterReset = `${dbName}.collShardedAfterReset`;
    const collShardedThenDropped = "collCreatedThenDropped";
    const nssShardedThenDropped = `${dbName}.${collShardedThenDropped}`;
    // Setup: shard 2 collections (expected to be captured by the snapshot read).
    assert.commandWorked(st.s.adminCommand({shardCollection: nssShardedBeforeReset, key: {_id: 1}}));
    assert.commandWorked(st.s.adminCommand({shardCollection: nssShardedThenDropped, key: {_id: 1}}));

    let resetPlacementHistoryRequest = launchAndPauseResetPlacementHistory();

    // Drop the first sharded collection and shard a new one; each operation is expected to
    // - be invisible to the snapshot read performed by resetPlacementHistory
    // - insert a placement change document with a greater timestamp (which won't be removed by the reset)
    assert.commandWorked(st.s.getDB(dbName).runCommand({drop: collShardedThenDropped}));
    assert.commandWorked(st.s.adminCommand({shardCollection: nssShardedAfterReset, key: {_id: 1}}));

    // Before resuming the execution, tag each the existing config.placementHistory documents for later inspection.
    assert.commandWorked(
        st.config.placementHistory.updateMany({}, {$set: {createdAfterReset: true}}, {writeConcern: {w: "majority"}}),
    );

    resetPlacementHistoryRequest.resumeAndJoin();

    // Pick the most recent placement history entry for each namespace in config.placementHistory
    let initializationTime = st.config.placementHistory
        .find({nss: initializationMetadataNssId})
        .sort({timestamp: -1})
        .limit(1)
        .next().timestamp;

    let placementChangesByNss = st.config.placementHistory
        .aggregate([
            {$match: {nss: {$ne: initializationMetadataNssId}}},
            {$sort: {timestamp: 1}},
            {$group: {_id: "$nss", placementChanges: {$push: "$$ROOT"}}},
        ])
        .toArray();

    function verifyPlacementDocForNamespace(nss, placementDoc, createdByResetCmd) {
        assert.eq(
            createdByResetCmd ? undefined : true,
            placementDoc.createdAfterReset,
            `The placement doc for ${nss} should ${createdByResetCmd ? "" : "not "}be part of the materialized view created by resetPlacementHistory`,
        );
        if (createdByResetCmd) {
            assert(timestampCmp(placementDoc.timestamp, initializationTime) <= 0);
        } else {
            assert(timestampCmp(placementDoc.timestamp, initializationTime) > 0);
        }
    }

    placementChangesByNss.forEach((nssGroup) => {
        switch (nssGroup._id) {
            // Namespaces that existed prior to the reset are expected to have a single placement change document,
            // created by the resetPlacementHistory command...
            case "config.system.sessions":
            case dbName:
            case nssShardedBeforeReset:
                assert.eq(
                    1,
                    nssGroup.placementChanges.length,
                    `Unexpected number of placement changes for nss ${nssGroup._id}`,
                );
                verifyPlacementDocForNamespace(nssGroup._id, nssGroup.placementChanges[0], true);
                break;
            // ... Except for the dropped collection, which should have two (the first generated by the reset while still existing, one generated by the dropCollection commit).
            case nssShardedThenDropped:
                assert.eq(
                    2,
                    nssGroup.placementChanges.length,
                    `Unexpected number of placement changes for nss ${nssGroup._id}`,
                );
                verifyPlacementDocForNamespace(nssGroup._id, nssGroup.placementChanges[0], true);
                assert.eq(1 /*the primary shard*/, nssGroup.placementChanges[0].shards.length);
                verifyPlacementDocForNamespace(nssGroup._id, nssGroup.placementChanges[1], false);
                assert.eq(0 /*no shards*/, nssGroup.placementChanges[1].shards.length);
                break;
            // Namespaces created after the reset are expected to have a single placement change document with a timestamp greater than the initialization time.
            case nssShardedAfterReset:
                assert.eq(
                    1,
                    nssGroup.placementChanges.length,
                    `Unexpected number of placement changes for nss ${nssGroup._id}`,
                );
                verifyPlacementDocForNamespace(nssGroup._id, nssGroup.placementChanges[0], false);
                break;
            default:
                assert(false, `Unexpected nss found in config.placementHistory: ${nssGroup._id}`);
        }
    });
}

{
    jsTest.log.info(
        "resetPlacementHistory is resilient to snapshot read errors due to the chosen PIT falling outside the max history window",
    );
    const dbName = "resilientExecutionTestDB";
    const nss = `${dbName}.coll`;

    assert.commandWorked(st.s.adminCommand({shardCollection: nss, key: {_id: 1}}));

    // Set a small 'snapshot history window' on the node that will execute the reset of 'config.placementHistory'
    // to more easily trigger the error scenario.
    const snapshotHistoryWindowSecs = 10;
    const testMarginSecs = 1;
    assert.commandWorked(
        configPrimary.adminCommand({setParameter: 1, minSnapshotHistoryWindowInSeconds: snapshotHistoryWindowSecs}),
    );

    let resetPlacementHistoryRequest = launchAndPauseResetPlacementHistory();

    // The operation has already picked up an initialization time for performing the snapshot read: retrieve its value from the recovery document of the supporting coordinator.
    const chosenInitializationTimeBeforeSnapshotReadError = configPrimary
        .getDB("config")
        .system.sharding_ddl_coordinators.findOne({
            "_id.operationType": "initializePlacementHistory",
        }).initializationTime;

    sleep((snapshotHistoryWindowSecs + testMarginSecs) * 1000);
    // Trigger the cleanup of the 'snapshot history' by creating a new database (which inserts a new document in config.databases with WC: majority).
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName + "_support"}));

    resetPlacementHistoryRequest.resumeAndJoin();

    // Pick the initialization time that was eventually set by the resetPlacementHistory operation;
    // this is expected to be strictly greater than the one picked before the snapshot read error.
    const finalInitializationTime = st.config.placementHistory
        .find({nss: initializationMetadataNssId})
        .sort({timestamp: -1})
        .limit(1)[0].timestamp;

    assert(
        timestampCmp(finalInitializationTime, chosenInitializationTimeBeforeSnapshotReadError) > 0,
        "Could not reproduce the expected error scenario",
    );
}

st.stop();
