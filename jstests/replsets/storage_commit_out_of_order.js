/**
 * Tests that single voting primaries can commit majority writes when they storage-commit out of
 * order. This test first inserts a document to set the last applied optime, all committed
 * timestamp, and stable timestamp. It then spawns 'n' threads and holds them behind a barrier. Once
 * the threads are all waiting at the barrier, the threads all do a w:majority insert. We turn on a
 * fail point that will block the first thread to receive an optime from the optime generator for a
 * few seconds while the other threads get later optimes and commit their inserts.  The hung thread
 * is released after a few seconds and asserts that its write concern can be satisfied.
 */
(function() {
    'use strict';

    load('jstests/libs/parallelTester.js');

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    const dbName = 'storage_commit_out_of_order';
    const collName = 'foo';
    const numThreads = 2;
    const primary = rst.getPrimary();
    const coll = primary.getDB(dbName).getCollection(collName);

    /**
     * Waits for the provided latch to reach 0 and then does a single w:majority insert.
     */
    const majorityInsert = function(num, host, dbName, collName, latch) {
        const m = new Mongo(host);
        latch.countDown();
        while (latch.getCount() > 0) {
            // do nothing
        }
        return m.getDB(dbName).runCommand({
            insert: collName,
            documents: [{b: num}],
            writeConcern: {w: 'majority', wtimeout: ReplSetTest.kDefaultTimeoutMS}
        });
    };

    assert.commandWorked(primary.setLogLevel(2, 'replication'));
    assert.commandWorked(coll.insert(
        {a: 1}, {writeConcern: {w: 'majority', wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

    // Turn on a fail point to force the first thread to receive an optime from the optime
    // generator to wait a few seconds before storage-committing the insert.
    assert.commandWorked(primary.adminCommand({
        configureFailPoint: 'sleepBetweenInsertOpTimeGenerationAndLogOp',
        mode: {times: 1},
        data: {waitForMillis: 3000}
    }));

    // Start a bunch of threads. They will block waiting on the latch to hit 0.
    const t = [];
    const counter = new CountDownLatch(numThreads + 1);
    for (let i = 0; i < numThreads; ++i) {
        t[i] = new ScopedThread(majorityInsert, i, coll.getMongo().host, dbName, collName, counter);
        t[i].start();
    }

    // Release the threads with the latch once they are all blocked on it.
    jsTestLog('All threads started.');
    assert.soon(() => counter.getCount() === 1);
    jsTestLog('All threads at barrier.');
    counter.countDown();
    jsTestLog('All threads finishing.');

    // Wait for all threads to complete and ensure they succeeded.
    for (let i = 0; i < numThreads; ++i) {
        t[i].join();
        assert.commandWorked(t[i].returnData());
    }

    rst.stopSet();
}());