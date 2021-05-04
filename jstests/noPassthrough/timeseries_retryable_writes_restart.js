/**
 * Tests time-series retryable writes oplog entries are correctly chained together so that a retry
 * after restarting the server doesn't perform a write that was already executed.
 *
 * @tags: [
 *     requires_replication,
 *     requires_persistence,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

function testRetryableRestart(ordered) {
    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();

    if (!TimeseriesTest.timeseriesCollectionsEnabled(primary)) {
        jsTestLog("Skipping test because the time-series collection feature flag is disabled");
        replTest.stopSet();
        return;
    }

    const testDB = primary.startSession({retryWrites: true}).getDatabase("test");
    const coll = testDB[jsTestName()];

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: "time", metaField: "meta"}}));

    function setupRetryableWritesForCollection(collName) {
        jsTestLog("Setting up the test collection");
        assert.commandWorked(coll.insert(
            [
                {time: ISODate(), x: 0, meta: 0},
                {time: ISODate(), x: 1, meta: 0},
                {time: ISODate(), x: 0, meta: 1},
                {time: ISODate(), x: 1, meta: 1},
            ],
            {writeConcern: {w: "majority"}}));

        const insertTag = "retryable insert " + collName;
        const updateTag = "retryable update " + collName;
        return {
            collName: collName,
            insertTag: insertTag,
            updateTag: updateTag,
            retryableInsertCommand: {
                insert: collName,
                documents: [
                    // Batched inserts resulting in "inserts".
                    {x: 0, time: ISODate(), tag: insertTag, meta: 2},
                    {x: 1, time: ISODate(), tag: insertTag, meta: 2},
                    {x: 0, time: ISODate(), tag: insertTag, meta: 3},
                    {x: 1, time: ISODate(), tag: insertTag, meta: 3},
                    // Batched inserts resulting in "updates".
                    {x: 2, time: ISODate(), tag: updateTag, meta: 0},
                    {x: 3, time: ISODate(), tag: updateTag, meta: 0},
                    {x: 2, time: ISODate(), tag: updateTag, meta: 1},
                    {x: 3, time: ISODate(), tag: updateTag, meta: 1},
                ],
                txnNumber: NumberLong(0),
                lsid: {id: UUID()},
                ordered: ordered,
            },
        };
    }

    function testRetryableWrites(writes) {
        const kCollName = writes.collName;
        jsTestLog("Testing retryable inserts");
        assert.commandWorked(testDB.runCommand(writes.retryableInsertCommand));
        // If retryable inserts don't work, we will see 8 here.
        assert.eq(4, testDB[kCollName].find({tag: writes.insertTag}).itcount());
        assert.eq(4, testDB[kCollName].find({tag: writes.updateTag}).itcount());
    }

    const retryableWrites = setupRetryableWritesForCollection(coll.getName());
    jsTestLog("Run retryable writes");
    assert.commandWorked(testDB.runCommand(retryableWrites.retryableInsertCommand));

    jsTestLog("Restarting the server to reconstruct retryable writes info");
    replTest.restart(primary);
    // Forces to block until the primary becomes writable.
    replTest.getPrimary();

    testRetryableWrites(retryableWrites);

    replTest.stopSet();
}

testRetryableRestart(true);
testRetryableRestart(false);
})();
