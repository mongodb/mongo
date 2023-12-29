/**
 * Test the basic operation of a `$search` aggregation stage.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB("test");
const coll = db.search;
coll.drop();

assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));
assert.commandWorked(coll.insert({"_id": 4, "title": "oranges"}));
assert.commandWorked(coll.insert({"_id": 5, "title": "cakes and oranges"}));
assert.commandWorked(coll.insert({"_id": 6, "title": "cakes and apples"}));
assert.commandWorked(coll.insert({"_id": 7, "title": "apples"}));
assert.commandWorked(coll.insert({"_id": 8, "title": "cakes and kale"}));

const collUUID = getUUIDFromListCollections(db, coll.getName());
const searchQuery = {
    query: "cakes",
    path: "title"
};
const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: "test"
};

// Give mongotmock some stuff to return.
{
    const cursorId = NumberLong(123);
    const history = [
        {
            expectedCommand: searchCmd,
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [
                        {_id: 1, $searchScore: 0.321},
                        {_id: 2, $searchScore: 0.654},
                        {_id: 5, $searchScore: 0.789}
                    ]
                },
                ok: 1
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 6, $searchScore: 0.123}]
                },
                ok: 1
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                ok: 1,
                cursor: {
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 8, $searchScore: 0.345}]
                },
            }
        },
    ];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
}

// Perform a $search query.
// Note that the 'batchSize' provided here only applies to the cursor between the driver and
// mongod, and has no effect on the cursor between mongod and mongotmock.
let cursor = coll.aggregate([{$search: searchQuery}], {cursor: {batchSize: 2}});

const expected = [
    {"_id": 1, "title": "cakes"},
    {"_id": 2, "title": "cookies and cakes"},
    {"_id": 5, "title": "cakes and oranges"},
    {"_id": 6, "title": "cakes and apples"},
    {"_id": 8, "title": "cakes and kale"}
];
assert.eq(expected, cursor.toArray());

// Simulate a case where mongot produces as an error after getting a search command.
{
    const history = [
        {
            expectedCommand: searchCmd,
            response: {
                ok: 0,
                errmsg: "mongot error",
                code: ErrorCodes.InternalError,
                codeName: "InternalError"
            }
        },
    ];

    assert.commandWorked(mongotConn.adminCommand(
        {setMockResponses: 1, cursorId: NumberLong(123), history: history}));
    const err = assert.throws(() => coll.aggregate([{$search: searchQuery}]));
    assert.commandFailedWithCode(err, ErrorCodes.InternalError);
}

// Simulate a case where mongot produces an error during a getMore.
{
    const cursorId = NumberLong(123);
    const history = [
        {
            expectedCommand: searchCmd,
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [
                        {_id: 1, $searchScore: 0.321},
                        {_id: 2, $searchScore: 0.654},
                        {_id: 3, $searchScore: 0.654},
                        {_id: 4, $searchScore: 0.789}
                    ]
                },
                ok: 1
            }
        },
        {
            expectedCommand: {getMore: cursorId, collection: coll.getName()},
            response: {
                ok: 0,
                errmsg: "mongot error",
                code: ErrorCodes.InternalError,
                codeName: "InternalError"
            }
        },
    ];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

    // The aggregate() (and search command) should succeed.
    // Note that 'batchSize' here only tells mongod how many docs to return per batch and has
    // no effect on the batches between mongod and mongotmock.
    const kBatchSize = 4;
    const cursor = coll.aggregate([{$search: searchQuery}], {batchSize: kBatchSize});

    // Iterate the first batch until it is exhausted.
    for (let i = 0; i < kBatchSize; i++) {
        cursor.next();
    }

    // The next call to next() will result in a 'getMore' being sent to mongod. $search's
    // internal cursor to mongot will have no results left, and thus, a 'getMore' will be sent
    // to mongot. The error should propagate back to the client.
    const err = assert.throws(() => cursor.next());
    assert.commandFailedWithCode(err, ErrorCodes.InternalError);
}

// Run $search on an empty collection.
{
    const cursor = db.doesNotExit.aggregate([{$search: searchQuery}]);
    assert.eq(cursor.toArray(), []);
}

// Fail on non-local read concern.
const err =
    assert.throws(() => coll.aggregate([{$search: {}}], {readConcern: {level: "majority"}}));
assert.commandFailedWithCode(err,
                             [ErrorCodes.InvalidOptions, ErrorCodes.ReadConcernMajorityNotEnabled]);

MongoRunner.stopMongod(conn);
mongotmock.stop();
