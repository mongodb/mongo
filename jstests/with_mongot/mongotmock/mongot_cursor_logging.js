/**
 * Test the debug information of the internal $search document source.
 *
 * @tags: [
 *   requires_fcv_71,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {prepCollection, prepMongotResponse} from "jstests/with_mongot/mongotmock/lib/utils.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const dbName = jsTestName();
const collName = jsTestName();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB(dbName);
db.setLogLevel(1);
db.setProfilingLevel(2);
const coll = db.getCollection(collName);
// When enabled, idLookup resolves _ids in batches: mongod drains mongot's whole cursor while
// building its first batch, instead of fetching one mongot batch per client getMore.
const batchedIdLookup = FeatureFlagUtil.isEnabled(db, "SearchOptimizedIdLookup");

prepCollection(conn, dbName, collName);

const collectionUUID = getUUIDFromListCollections(db, coll.getName());
let cursorId = NumberLong(123);

function runTest(pipeline, expectedCommand) {
    const expected = prepMongotResponse(expectedCommand, coll, mongotConn, cursorId);

    // Perform a $search/$vectorSearch query.
    // Note that the 'batchSize' provided here only applies to the cursor between the driver and
    // mongod, and has no effect on the cursor between mongod and mongotmock.
    let cursor = coll.aggregate(pipeline, {cursor: {batchSize: 2}});

    assert.eq(expected, cursor.toArray());
    const log = assert.commandWorked(db.adminCommand({getLog: "global"})).log;
    function containsMongotCursor(logLine) {
        // Count only completed "Slow query" lines. On slow/debug builds a long-running aggregate is
        // also logged mid-flight as "Slow in-progress query", which would double-count the single
        // batched round-trip.
        return (
            logLine.includes(`\"cursorid\":${cursorId.valueOf()},`) &&
            !logLine.includes('"msg":"Slow in-progress query"')
        );
    }
    const mongotCursorLog = log.filter(containsMongotCursor);
    const batchNumRegex =
        /"mongot":{"cursorid":[0-9]+,"timeWaitingMillis":[0-9]+,"batchNum":([0-9]+)}/;
    if (batchedIdLookup) {
        // idLookup drains mongot's whole cursor (all three batches) while building mongod's first
        // batch, so the interaction is logged and profiled once, tagged with the final batch num.
        assert.eq(mongotCursorLog.length, 1, tojson(log));
        const regexMatch = batchNumRegex.exec(mongotCursorLog[0]);
        assert.eq(regexMatch[1], 3, mongotCursorLog[0]);

        const profilerLog = db.system.profile.find({"mongot.cursorid": cursorId}).toArray();
        assert.eq(profilerLog.length, 1, tojson(profilerLog));
    } else {
        // mongod fetches one mongot batch per client getMore, so all three batches are logged and
        // profiled separately, in order.
        assert.eq(mongotCursorLog.length, 3, tojson(log));
        let expectedBatchNum = 1;
        mongotCursorLog.forEach(function (element) {
            let regexMatch = batchNumRegex.exec(element);
            assert.eq(regexMatch.length, 2, element + " - regex - " + batchNumRegex);
            // Take advantage of being able to compare strings to ints.
            assert.eq(
                regexMatch[1],
                expectedBatchNum,
                "Expected batch number " + expectedBatchNum + " but found " + regexMatch[1],
            );
            expectedBatchNum++;
        });

        const profilerLog = db.system.profile.find({"mongot.cursorid": cursorId}).toArray();
        assert.eq(profilerLog.length, 3, tojson(profilerLog));
    }

    // Increment the cursor so that the next test will have a unique value.
    cursorId = NumberLong(cursorId + 1);
}

const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    limit: 5,
};
runTest(
    [{$vectorSearch: vectorSearchQuery}],
    mongotCommandForVectorSearchQuery({...vectorSearchQuery, collName, dbName, collectionUUID}),
);

const searchQuery = {
    query: "cakes",
    path: "title",
};
runTest([{$search: searchQuery}], {
    search: collName,
    collectionUUID,
    query: searchQuery,
    $db: dbName,
});

MongoRunner.stopMongod(conn);
mongotmock.stop();
