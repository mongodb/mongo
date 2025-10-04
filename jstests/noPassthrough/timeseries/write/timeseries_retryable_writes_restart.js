/**
 * Tests time-series retryable writes oplog entries are correctly chained together so that a retry
 * after restarting the server doesn't perform a write that was already executed.
 *
 * @tags: [
 *     requires_replication,
 *     requires_persistence,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const timeFieldName = "time";
const metaFieldName = "m";

function testRetryableRestart(ordered) {
    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const primary = replTest.getPrimary();

    const testDB = primary.startSession({retryWrites: true}).getDatabase("test");
    const coll = testDB[jsTestName()];

    assert.commandWorked(
        testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    function setupRetryableWritesForCollection(collName) {
        jsTestLog("Setting up the test collection");
        assert.commandWorked(
            coll.insert(
                [
                    {[timeFieldName]: ISODate(), x: 0, [metaFieldName]: 0},
                    {[timeFieldName]: ISODate(), x: 1, [metaFieldName]: 0},
                    {[timeFieldName]: ISODate(), x: 0, [metaFieldName]: 1},
                    {[timeFieldName]: ISODate(), x: 1, [metaFieldName]: 1},
                ],
                {writeConcern: {w: "majority"}},
            ),
        );

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
                    {x: 0, [timeFieldName]: ISODate(), tag: insertTag, [metaFieldName]: 2},
                    {x: 1, [timeFieldName]: ISODate(), tag: insertTag, [metaFieldName]: 2},
                    {x: 0, [timeFieldName]: ISODate(), tag: insertTag, [metaFieldName]: 3},
                    {x: 1, [timeFieldName]: ISODate(), tag: insertTag, [metaFieldName]: 3},
                    // Batched inserts resulting in "updates".
                    {x: 2, [timeFieldName]: ISODate(), tag: updateTag, [metaFieldName]: 0},
                    {x: 3, [timeFieldName]: ISODate(), tag: updateTag, [metaFieldName]: 0},
                    {x: 2, [timeFieldName]: ISODate(), tag: updateTag, [metaFieldName]: 1},
                    {x: 3, [timeFieldName]: ISODate(), tag: updateTag, [metaFieldName]: 1},
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
