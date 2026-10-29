/**
 * Parks a $search cursor, runs collMod, then issues getMore.
 *
 * With featureFlagSearchOptimizedIdLookup, $_internalSearchIdLookup reuses a collection
 * acquisition that is yielded while the cursor is parked. getMore must restore those
 * resources before checking the collection UUID (SERVER-134004).
 *
 * 'internalSearchIdLookupMaxBatchSize' is 1 so the second _id is looked up on getMore, not
 * during the first aggregate batch.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_getmore,
 *   does_not_support_stepdowns,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({
    setParameter: {
        mongotHost: mongotConn.host,
        featureFlagSearchOptimizedIdLookup: true,
        internalSearchIdLookupMaxBatchSize: 1,
    },
});
const db = conn.getDB(jsTestName());
const coll = db.getCollection(jsTestName());

assert.commandWorked(coll.insert({_id: 0}));
assert.commandWorked(coll.insert({_id: 1}));

const searchQuery = {
    query: "query",
    path: "title",
};
assert.commandWorked(
    mongotConn.adminCommand({
        setMockResponses: 1,
        cursorId: NumberLong(123),
        history: [
            {
                expectedCommand: {
                    search: coll.getName(),
                    collectionUUID: getUUIDFromListCollections(db, coll.getName()),
                    query: searchQuery,
                    $db: db.getName(),
                },
                response: {
                    cursor: {
                        id: NumberLong(0),
                        ns: coll.getFullName(),
                        nextBatch: [
                            {_id: 0, $searchScore: 1},
                            {_id: 1, $searchScore: 0.9},
                        ],
                    },
                    ok: 1,
                },
            },
        ],
    }),
);

const res = assert.commandWorked(
    db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$search: searchQuery}],
        cursor: {batchSize: 1},
    }),
);
assert.eq(1, res.cursor.firstBatch.length, "expected a parked cursor after the first batch", {res});
assert.neq(NumberLong(0), res.cursor.id, "expected a non-zero cursor id", {res});

assert.commandWorked(db.runCommand({collMod: coll.getName(), validationLevel: "strict"}));
assert.commandWorked(
    db.runCommand({getMore: res.cursor.id, collection: coll.getName(), batchSize: 1}),
);

db.runCommand({killCursors: coll.getName(), cursors: [res.cursor.id]});

MongoRunner.stopMongod(conn);
mongotmock.stop();
