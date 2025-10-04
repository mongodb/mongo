/**
 * Verify that '$$SEARCH_META' behaves correctly if empty in the results from mongot.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB("test");
const coll = db.searchCollector;
coll.drop();
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));

const collUUID = getUUIDFromListCollections(db, coll.getName());
const searchQuery = {
    query: "cakes",
    path: "title",
};

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

// Verify that a $search query SEARCH_META evaluates to missing if no collector results are returned
// from mongot.
let cursor = coll.aggregate([{$search: searchQuery}, {$project: {_id: 1, meta: "$$SEARCH_META"}}], {cursor: {}});

const expected = [{"_id": 2}, {"_id": 1}, {"_id": 3}];
assert.eq(expected, cursor.toArray());

MongoRunner.stopMongod(conn);
mongotmock.stop();
