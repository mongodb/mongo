/**
 * Verify that `$searchBeta` works as an alias for `$search`.
 */
import {getMatchingLoglinesCount} from "jstests/libs/log.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB("test");
const coll = db.searchBeta;
coll.drop();

assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));

const collUUID = getUUIDFromListCollections(db, coll.getName());
const searchQuery = {
    query: "cakes",
    path: "title",
};

// Verify that $searchBeta parses with no errors..
{
    const cursor = db.doesNotExist.aggregate([{$searchBeta: searchQuery}]);
    assert.eq(cursor.toArray(), []);
}

const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: "test",
};

// Give mongotmock some stuff to return.
const cursorId = NumberLong(123);

{
    const history = [
        {
            expectedCommand: searchCmd,
            response: {
                ok: 1,
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [
                        {_id: 2, $searchScore: 0.654},
                        {_id: 1, $searchScore: 0.321},
                    ],
                },
            },
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 3, $searchScore: 0.123}],
                },
                ok: 1,
            },
        },
    ];

    assert.commandWorked(mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
}

// Verify that a $searchBeta query works end to end.
// Note that the 'batchSize' provided here only applies to the cursor between the driver and
// mongod, and has no effect on the cursor between mongod and mongotmock.
let cursor = coll.aggregate([{$searchBeta: searchQuery}], {cursor: {batchSize: 2}});

const expected = [
    {"_id": 2, "title": "cookies and cakes"},
    {"_id": 1, "title": "cakes"},
    {"_id": 3, "title": "vegetables"},
];
assert.eq(expected, cursor.toArray());

// Verify that a deprecation warning was logged for $searchBeta usage.
const globalLog = assert.commandWorked(db.adminCommand({getLog: "global"}));
const count = getMatchingLoglinesCount(globalLog.log, {msg: "$searchBeta is deprecated. Use $search instead."});
assert.eq(count, 1, "Expected exactly one deprecation warning for $searchBeta");

MongoRunner.stopMongod(conn);
mongotmock.stop();
