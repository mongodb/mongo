/**
 * Test that mongotmock gets a kill cursor command when the cursor is killed on mongod.
 *
 * Test requires FCV 8.1 since batchSize tuning (enabled in 8.1) changes the prefetch logic, which
 * changes the expected cursor history.
 * @tags: [ requires_fcv_81 ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForQuery,
    mongotCommandForVectorSearchQuery,
    MongotMock,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {prepCollection} from "jstests/with_mongot/mongotmock/lib/utils.js";

const mongotMock = new MongotMock();
mongotMock.start();

const collName = jsTestName();
const dbName = "mongotTest";

const mongotConn = mongotMock.getConnection();
const mongotTestDB = mongotConn.getDB(dbName);

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB(dbName);
// When enabled, idLookup resolves _ids a whole window at a time. An unbounded query then drains
// mongot's cursor to EOF (nothing left to kill), so the killCursors-forwarding scenario is only
// reachable when a limit stops the drain early and leaves the cursor open.
const batchedIdLookup = FeatureFlagUtil.isEnabled(db, "SearchOptimizedIdLookup");

const coll = db.getCollection(collName);

prepCollection(conn, dbName, collName);

const cursorId = NumberLong(123);
const collectionUUID = getUUIDFromListCollections(db, coll.getName());

function runTest(pipeline, expectedCommand, shouldPrefetchGetMore, limit) {
    const someScore = {$vectorSearchScore: 0.97};
    let firstBatch;
    if (batchedIdLookup) {
        // Return exactly the limit's worth of docs in the first batch. idLookup pulls the whole
        // window at once, meets the limit without a getMore, and stops with the cursor still open.
        firstBatch = [];
        for (let i = 0; i < limit; i++) {
            firstBatch.push({_id: i + 1, ...someScore});
        }
    } else {
        firstBatch = [
            {_id: 1, ...someScore},
            {_id: 2, ...someScore},
        ];
    }
    const cursorHistory = [
        {
            expectedCommand,
            response: {
                ok: 1,
                cursor: {
                    firstBatch,
                    id: cursorId,
                    ns: coll.getFullName(),
                },
            },
        },
    ];
    // After the first batch is exhausted, the mongot-remote stage prefetches one getMore (whose
    // results go unused here) while the cursor stays open. Batched, idLookup has already met its
    // limit from the first window, so this prefetch is the only getMore issued.
    if (shouldPrefetchGetMore) {
        cursorHistory.push({
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 14, ...someScore}],
                },
                ok: 1,
            },
        });
    }
    cursorHistory.push({
        expectedCommand: {killCursors: coll.getName(), cursors: [cursorId]},
        response: {
            cursorsKilled: [cursorId],
            cursorsNotFound: [],
            cursorsAlive: [],
            cursorsUnknown: [],
            ok: 1,
        },
    });

    assert.commandWorked(
        mongotTestDB.runCommand({setMockResponses: 1, cursorId: cursorId, history: cursorHistory}),
    );

    // Perform a query that creates a cursor on mongot.
    // Note that the 'batchSize' provided here only applies to the cursor between the driver and
    // mongod, and has no effect on the cursor between mongod and mongotmock.
    let cursor = coll.aggregate(pipeline, {cursor: {batchSize: 2}});

    // Call killCursors on the mongod cursor.
    cursor.close();

    // Make sure killCursors was called on mongot. We cannot assume that this happens immediately
    // after cursor.close() since mongod's killCursors command to mongot may race with the shell's
    // getQueuedResponses command to mongot.
    assert.soon(function () {
        let resp = assert.commandWorked(mongotTestDB.runCommand({getQueuedResponses: 1}));
        return resp.numRemainingResponses === 0;
    });
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
    /*shouldPrefetchGetMore*/ true,
    /*limit*/ vectorSearchQuery.limit,
);

const searchQuery = {
    query: "cakes",
    path: "title",
};
// This unbounded $search has no limit to stop a batched drain early, so under batched idLookup it
// would run mongot's cursor to EOF and leave nothing to kill. The killCursors-forwarding path is
// covered by the limited $vectorSearch case above.
if (!batchedIdLookup) {
    runTest(
        [{$search: searchQuery}],
        mongotCommandForQuery({
            query: searchQuery,
            collName: collName,
            db: dbName,
            collectionUUID: collectionUUID,
        }),
        /*shouldPrefetchGetMore*/ false,
        /*limit*/ 2,
    );
}

mongotMock.stop();
MongoRunner.stopMongod(conn);
