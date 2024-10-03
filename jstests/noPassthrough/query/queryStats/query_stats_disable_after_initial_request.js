/**
 * Tests that query stats aren't collected if the feature is enabled initially but is disabled
 * before the lifetime of the request is complete.
 * @tags: [
 *   # Query Stats is new in 7.1.
 *   requires_fcv_71,
 * ]
 */
import {getQueryStatsFindCmd} from "jstests/libs/query/query_stats_utils.js";

// Test that no query stats entry is written when (1) dispatching an initial find query, (2)
// disabling query stats, then (3) completing the command. Below, we run variations of this test
// with combinations of different strategies to disable query stats and to end the command.
function testStatsAreNotCollectedWhenDisabledBeforeCommandCompletion(
    {conn, coll, disableQueryStatsFn, endCommandFn, enableQueryStatsFn}) {
    // Issue a find commannd with a batchSize of 1 so that the query is not exhausted.
    const cursor = coll.find({foo: 1}).batchSize(1);
    // Must run .next() to make sure the initial request is executed now.
    cursor.next();

    // Disable query stats, then end the command,which triggers the path to writeQueryStats.
    disableQueryStatsFn();
    endCommandFn(cursor);

    // Must re-enable query stats in order to check via $queryStats that nothing was recorded.
    enableQueryStatsFn();
    const res = getQueryStatsFindCmd(conn);
    assert.eq(res.length, 0, res);
}

// Turn on the collecting of queryStats metrics.
let options = {setParameter: {internalQueryStatsRateLimit: -1}};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 100;
for (let i = 0; i < numDocs / 2; ++i) {
    bulk.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
    bulk.insert({foo: 1, bar: Math.floor(Math.random() * -2)});
}
assert.commandWorked(bulk.execute());

function setQueryStatsCacheSize(size) {
    assert.commandWorked(testDB.adminCommand({setParameter: 1, internalQueryStatsCacheSize: size}));
}

// Tests the scenario of disabling query stats by setting internalQueryStatsCacheSize to 0 and
// ending the command by running it to completion.
testStatsAreNotCollectedWhenDisabledBeforeCommandCompletion({
    conn: testDB,
    coll,
    disableQueryStatsFn: () => setQueryStatsCacheSize("0MB"),
    endCommandFn: (cursor) => cursor.itcount(),
    enableQueryStatsFn: () => setQueryStatsCacheSize("10MB")
});

// Tests the scenario of disabling query stats by setting internalQueryStatsCacheSize to 0 and
// ending the command by killing the cursor.
testStatsAreNotCollectedWhenDisabledBeforeCommandCompletion({
    conn: testDB,
    coll,
    disableQueryStatsFn: () => setQueryStatsCacheSize("0MB"),
    endCommandFn: (cursor) => assert.commandWorked(
        testDB.runCommand({killCursors: coll.getName(), cursors: [cursor.getId()]})),
    enableQueryStatsFn: () => setQueryStatsCacheSize("10MB")
});

MongoRunner.stopMongod(conn);
