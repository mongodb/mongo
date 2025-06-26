/**
 * Tests server behavior when a StorageUnavailableException is thrown when a PlanExecutor is being
 * restored.  Specifically, tests the case where the exception is thrown when the getMore command
 * is restoring the cursor in order to use it, as well as a case where the BatchedDeleteStage
 * throws when restoring.
 */

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB("test");

function runTest() {
    load("jstests/libs/fail_point_util.js");

    let collName = jsTestName();
    db[collName].drop();

    for (let x = 0; x < 5; ++x) {
        assert.commandWorked(db[collName].insert({_id: x, a: 1}));
    }

    assert.commandWorked(db[collName].createIndex({a: 1}));

    //
    // Test find command.
    //
    let res = db.runCommand({find: collName, filter: {a: 1}, batchSize: 1});
    assert.eq(1, res.cursor.firstBatch.length, tojson(res));

    // Configure the failpoint to trip once, when the getMore command restores the cursor.
    let fp1 = configureFailPoint(db, "throwDuringIndexScanRestore", {} /* data */, {times: 1});

    let getMoreRes =
        assert.commandWorked(db.runCommand({getMore: res.cursor.id, collection: collName}));
    assert.eq(4, getMoreRes.cursor.nextBatch.length, tojson(getMoreRes));

    //
    // Test aggregate command.
    //
    res =
        db.runCommand({aggregate: collName, pipeline: [{$match: {a: 1}}], cursor: {batchSize: 1}});
    assert.eq(1, res.cursor.firstBatch.length, tojson(res));

    // Configure the failpoint to trip once, when the getMore command restores the cursor.
    let fp2 = configureFailPoint(db, "throwDuringIndexScanRestore", {} /* data */, {times: 1});
    getMoreRes =
        assert.commandWorked(db.runCommand({getMore: res.cursor.id, collection: collName}));
    assert.eq(4, getMoreRes.cursor.nextBatch.length, tojson(getMoreRes));

    //
    // Test batched delete command.
    //

    // Configure the fail point to trip once.
    let fp3 = configureFailPoint(
        db, "batchedDeleteStageThrowWriteConflictException", {} /* data */, {times: 1});
    assert.commandWorked(db[collName].remove({}));
}

runTest();

MongoRunner.stopMongod(conn);
