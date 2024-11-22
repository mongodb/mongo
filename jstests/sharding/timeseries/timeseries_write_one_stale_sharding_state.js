/**
 * Tests two-phase write commands on a timeseries collection when the sharding state is stale.
 *
 * @tags: [
 *   featureFlagTimeseriesUpdatesSupport,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *    # TODO (SERVER-88125): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */

import {
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
    generateTimeValue,
    getCallerName,
    mongos0DB,
    mongos1DB,
    prepareCollection,
    prepareShardedCollection,
    setUpShardedCluster,
    tearDownShardedCluster
} from "jstests/core/timeseries/libs/timeseries_writes_util.js";

const docs = [
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
];

function verifyUpdateDeleteOneRes(res, nAffected) {
    assert.eq(nAffected, res.n, tojson(res));
}

function verifyFindAndModifyRes(res, nAffected, resultDoc) {
    assert.eq(nAffected, res.lastErrorObject.n, tojson(res));
    assert.docEq(resultDoc, res.value, tojson(res));
}

/**
 * Verifies that a write one command succeed or fail with the expected error code when the sharding
 * state is stale.
 */
function testWriteOneOnCollectionWithStaleShardingState({
    writeCmd,
    nAffected,
    resultDoc,
}) {
    const callerName = getCallerName();
    jsTestLog(`Running ${callerName}(${tojson(arguments[0])})`);

    let isFindAndModifyCmd = false;
    // The collection name is same as the caller name.
    const collName = (() => {
        if (writeCmd.hasOwnProperty("findAndModify")) {
            isFindAndModifyCmd = true;
            writeCmd["findAndModify"] = callerName;
            return writeCmd["findAndModify"];
        } else if (writeCmd.hasOwnProperty("delete") && writeCmd["deletes"].length === 1 &&
                   writeCmd["deletes"][0].limit === 1) {
            writeCmd["delete"] = callerName;
            return writeCmd["delete"];
        } else if (writeCmd.hasOwnProperty("update") && writeCmd["updates"].length === 1 &&
                   !writeCmd["updates"][0].multi) {
            writeCmd["update"] = callerName;
            return writeCmd["update"];
        } else {
            assert(false, "Unsupported write command");
        }
    })();

    // Prepares an unsharded collection on mongos1 which will be soon sharded and then mongos1 will
    // have a stale sharding state.
    prepareCollection({dbToUse: mongos1DB, collName: collName, initialDocList: docs});

    // Creates and shards a timeseries collection on mongos0.
    prepareShardedCollection({dbToUse: mongos0DB, collName: collName, initialDocList: docs});

    // This write command should succeed though mongos1 has a stale sharding state since the mongos1
    // should be able to refresh its sharding state from the config server and retry the write
    // command internally.
    let res = assert.commandWorked(mongos1DB[collName].runCommand(writeCmd));
    if (isFindAndModifyCmd) {
        verifyFindAndModifyRes(res, nAffected, resultDoc);
    } else {
        verifyUpdateDeleteOneRes(res, nAffected);
    }

    // This will cause mongos1 to have the up-to-date sharding state but this state will be soon
    // stale again.
    mongos1DB[collName].insert(resultDoc);

    // Drops and recreates the collection on mongos0.
    prepareCollection({dbToUse: mongos0DB, collName: collName, initialDocList: docs});

    // This write command will fail because mongos1 has a stale sharding state.
    res = assert.commandFailedWithCode(mongos1DB[collName].runCommand(writeCmd),
                                       ErrorCodes.NamespaceNotSharded);

    // This write command should succeed since mongos1 should have refreshed its sharding state.
    res = assert.commandWorked(mongos1DB[collName].runCommand(writeCmd));
    jsTestLog(tojson(res));
    if (isFindAndModifyCmd) {
        verifyFindAndModifyRes(res, nAffected, resultDoc);
    } else {
        verifyUpdateDeleteOneRes(res, nAffected);
    }
}

setUpShardedCluster({nMongos: 2});

(function testFindOneAndRemoveOnCollectionWithStaleShardingState() {
    testWriteOneOnCollectionWithStaleShardingState({
        writeCmd: {findAndModify: "$$$", query: {f: 101}, remove: true},
        nAffected: 1,
        resultDoc: doc2_a_f101,
    });
})();

(function testDeleteOneOnCollectionWithStaleShardingState() {
    testWriteOneOnCollectionWithStaleShardingState({
        writeCmd: {delete: "$$$", deletes: [{q: {f: 105}, limit: 1}]},
        nAffected: 1,
        resultDoc: doc6_c_f105,
    });
})();

(function testFindOneAndUpdateOnCollectionWithStaleShardingState() {
    testWriteOneOnCollectionWithStaleShardingState({
        writeCmd: {findAndModify: "$$$", query: {f: 106}, update: {$set: {f: 107}}},
        nAffected: 1,
        resultDoc: doc7_c_f106,
    });
})();

(function testUpdateOneOnCollectionWithStaleShardingState() {
    testWriteOneOnCollectionWithStaleShardingState({
        writeCmd: {update: "$$$", updates: [{q: {f: 106}, u: {$set: {f: 107}}, multi: false}]},
        nAffected: 1,
        resultDoc: doc7_c_f106,
    });
})();

(function testFindAndModifyUpsertOnCollectionWithStaleShardingState() {
    const replacementDoc = {_id: 1000, tag: "A", time: generateTimeValue(0), f: 1000};
    testWriteOneOnCollectionWithStaleShardingState({
        writeCmd: {
            findAndModify: "$$$",
            query: {f: 1000},
            update: replacementDoc,
            upsert: true,
            new: true
        },
        nAffected: 1,
        resultDoc: replacementDoc,
    });
})();

(function testUpdateOneUpsertOnCollectionWithStaleShardingState() {
    const replacementDoc = {_id: 1000, tag: "A", time: generateTimeValue(0), f: 1000};
    testWriteOneOnCollectionWithStaleShardingState({
        writeCmd: {
            update: "$$$",
            updates: [{q: {f: 1000}, u: replacementDoc, multi: false, upsert: true}]
        },
        nAffected: 1,
        resultDoc: replacementDoc,
    });
})();

tearDownShardedCluster();
