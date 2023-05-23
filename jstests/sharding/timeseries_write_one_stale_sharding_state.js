/**
 * Tests two-phase write commands on a timeseries collection when the sharding state is stale.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # To avoid burn-in tests in in-memory build variants
 *   requires_persistence,
 *   # 'NamespaceNotSharded' error is supported since 7.1
 *   requires_fcv_71,
 *   featureFlagUpdateOneWithoutShardKey,
 * ]
 */

(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries_writes_util.js");

const docs = [
    doc1_a_nofields,
    doc2_a_f101,
    doc3_a_f102,
    doc4_b_f103,
    doc5_b_f104,
    doc6_c_f105,
    doc7_c_f106,
];

function verifyDeleteOneRes(res, nAffected) {
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

    let findAndModifyCmd = false;
    let deleteOneCmd = false;
    let updateOneCmd = false;
    // The collection name is same as the caller name.
    const collName = (() => {
        if (writeCmd.hasOwnProperty("findAndModify")) {
            findAndModifyCmd = true;
            writeCmd["findAndModify"] = callerName;
            return writeCmd["findAndModify"];
        } else if (writeCmd.hasOwnProperty("delete") && writeCmd["deletes"].length === 1 &&
                   writeCmd["deletes"][0].limit === 1) {
            deleteOneCmd = true;
            writeCmd["delete"] = callerName;
            return writeCmd["delete"];
        } else if (writeCmd.hasOwnProperty("update") && writeCmd["updates"].length === 1 &&
                   !writeCmd["updates"][0].multi) {
            updateOneCmd = true;
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
    if (deleteOneCmd) {
        verifyDeleteOneRes(res, nAffected);
    } else if (findAndModifyCmd) {
        verifyFindAndModifyRes(res, nAffected, resultDoc);
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
    if (deleteOneCmd) {
        verifyDeleteOneRes(res, nAffected);
    } else if (findAndModifyCmd) {
        verifyFindAndModifyRes(res, nAffected, resultDoc);
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

// TODO SERVER-76871: Add tests for updateOne update and findAndModify update/upsert.
// TODO SERVER-77132: Add tests for updateOne upsert.

tearDownShardedCluster();
})();
