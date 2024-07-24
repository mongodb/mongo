/**
 * Test that mongotmock gets a kill cursor command when the cursor is killed on mongod.
 *
 * @tags: [
 *   requires_fcv_71,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    mongotCommandForVectorSearchQuery,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    prepCollection,
} from "jstests/with_mongot/mongotmock/lib/utils.js";

const mongotMock = new MongotMock();
mongotMock.start();

const collName = jsTestName();
const dbName = "mongotTest";

const mongotConn = mongotMock.getConnection();
const mongotTestDB = mongotConn.getDB(dbName);

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB(dbName);

const coll = db.getCollection(collName);

prepCollection(conn, dbName, collName);

const cursorId = NumberLong(123);
const collectionUUID = getUUIDFromListCollections(db, coll.getName());

function runTest(pipeline, expectedCommand) {
    const cursorHistory = [
        {
            expectedCommand,
            response: {
                ok: 1,
                cursor: {firstBatch: [{_id: 1}, {_id: 2}], id: cursorId, ns: coll.getFullName()}
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response:
                {cursor: {id: cursorId, ns: coll.getFullName(), nextBatch: [{_id: 14}]}, ok: 1}
        },
        {
            expectedCommand: {killCursors: coll.getName(), cursors: [cursorId]},
            response: {
                cursorsKilled: [cursorId],
                cursorsNotFound: [],
                cursorsAlive: [],
                cursorsUnknown: [],
                ok: 1,
            }
        }
    ];

    assert.commandWorked(
        mongotTestDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: cursorHistory}));

    // Perform a query that creates a cursor on mongot.
    // Note that the 'batchSize' provided here only applies to the cursor between the driver and
    // mongod, and has no effect on the cursor between mongod and mongotmock.
    let cursor = coll.aggregate(pipeline, {cursor: {batchSize: 2}});

    // Call killCursors on the mongod cursor.
    cursor.close();

    // Make sure killCursors was called on mongot. We cannot assume that this happens immediately
    // after cursor.close() since mongod's killCursors command to mongot may race with the shell's
    // getQueuedResponses command to mongot.
    assert.soon(function() {
        let resp = assert.commandWorked(mongotTestDB.runCommand({getQueuedResponses: 1}));
        return resp.numRemainingResponses === 0;
    });
}

const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    limit: 5
};
runTest(
    [{$vectorSearch: vectorSearchQuery}],
    mongotCommandForVectorSearchQuery({...vectorSearchQuery, collName, dbName, collectionUUID}));

const searchQuery = {
    query: "cakes",
    path: "title"
};
runTest([{$search: searchQuery}],
        mongotCommandForQuery(
            {query: searchQuery, collName: collName, db: dbName, collectionUUID: collectionUUID}));

mongotMock.stop();
MongoRunner.stopMongod(conn);
