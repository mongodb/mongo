/* This test checks that currentOp correctly reports the lockStats for sub-operations. Before
 * SERVER-26854, the currentOp entry for each sub-operation reports the aggregate lock wait time of
 * all preceding sub-operations.
 *
 * To test the correctness of currentOp entry for sub-operations, we use an aggregation pipeline
 * which can trigger a series of sub-operations: createCollection, createIndex, listCollections,
 * listIndexes, renameCollection and dropCollection.
 *
 * Test steps:
 *  1. Lock the database by FsyncLock.
 *  2. Turn 'hangAfterIndexBuild' failpoint on.
 *  3. Run the aggregation, which will get blocked on sub-operation: {'create' : 'tmp.agg_out.x'}.
 *  4. Sleep for 2 seconds.
 *  5. FsyncUnlock. Now the next sub-operation createIndex gets blocked by the failpoint.
 *  6. Run 'currentOp' to check the entry for createIndex. The lock wait time should be 0 rather
 *     than ~2.
 *
 * @tags: [requires_fsync]
 */
(function() {
    'use strict';

    const conn = MongoRunner.runMongod();
    const db = conn.getDB('test');
    const coll = db.books;
    const blockedMillis = 2000;
    assert.writeOK(coll.insert({title: 'Adventures of Huckleberry'}));
    assert.writeOK(coll.insert({title: '1984'}));
    assert.writeOK(coll.insert({title: 'Animal Farm'}));
    // Create the output collection beforehand so that $out will execute a code path which triggers
    // the index creation sub-operation.
    db['favorite'].createIndex({foo: 1});

    db.setProfilingLevel(0, -1);

    // Lock the database, and then start an operation that needs the lock, so it blocks.
    assert.commandWorked(db.fsyncLock());

    // Turn 'hangAfterStartingIndexBuildUnlocked' failpoint on, which blocks any index builds.
    assert.commandWorked(
        db.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'alwaysOn'}));

    // Aggregation with $out which will block on creating the temporary collection due to the
    // FsyncLock.
    const dollarOutAggregationShell = startParallelShell(function() {
        // Simple aggregation which copies a document to the output collection.
        assert.commandWorked(db.runCommand({
            aggregate: 'books',
            pipeline: [{$match: {title: '1984'}}, {$out: 'favorite'}],
            cursor: {}
        }));
    }, conn.port);

    // Wait for sub-operation createCollection to get blocked.
    assert.soon(function() {
        let res = db.currentOp({"command.create": {$exists: true}, waitingForLock: true});
        return res.inprog.length == 1;
    });

    sleep(blockedMillis);

    // Unlock the database. Sub-operation createCollection can proceed.
    db.fsyncUnlock();

    // Wait for sub-operation createIndex to get blocked.
    let res;
    assert.soon(function() {
        res = db.currentOp({"msg": "Index Build"});
        return res.inprog.length == 1;
    });
    jsTestLog(tojson(res.inprog[0]));
    // Assert that sub-operation 'createIndex' has 0 lock wait time. Before SERVER-26854, it
    // erroneously reported ~2s as it counted the lock wait time for the previous sub-operation.
    assert(!('timeAcquiringMicros' in res.inprog[0].lockStats.Global));

    assert.commandWorked(
        db.adminCommand({configureFailPoint: 'hangAfterStartingIndexBuild', mode: 'off'}));
    dollarOutAggregationShell();
    MongoRunner.stopMongod(conn);
})();
