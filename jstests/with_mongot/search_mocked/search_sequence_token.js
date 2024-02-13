/**
 * Test the basic case for searchSequenceToken in standalone environment with classic engine.
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

assert.commandWorked(coll.insert({"_id": 1, "test": 100}));
assert.commandWorked(coll.insert({"_id": 2, "test": 100}));
assert.commandWorked(coll.insert({"_id": 3, "test": 1}));
assert.commandWorked(coll.insert({"_id": 4, "test": 10}));
assert.commandWorked(coll.insert({"_id": 5, "test": 100}));
assert.commandWorked(coll.insert({"_id": 6, "test": 100}));
assert.commandWorked(coll.insert({"_id": 7, "test": 10}));
assert.commandWorked(coll.insert({"_id": 8, "test": 100}));

const collUUID = getUUIDFromListCollections(db, coll.getName());
const searchQuery = {
    query: "cakes",
    path: "title"
};
const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: "test",
    cursorOptions: {requiresSearchSequenceToken: true}
};

const cursorId = NumberLong(123);
const history = [
    {
        expectedCommand: searchCmd,
        response: {
            cursor: {
                id: cursorId,
                ns: coll.getFullName(),
                nextBatch: [
                    {
                        _id: 1,
                        $searchSequenceToken: "aaaaaaa==",
                        $searchScore: 1.234,
                    },
                    {
                        _id: 2,
                        $searchSequenceToken: "bbbbbbb==",
                        $searchScore: 1.345,
                    },
                    {
                        _id: 3,
                        $searchSequenceToken: "ccccccc==",
                        $searchScore: 2.234,
                    },
                    {
                        _id: 4,
                        $searchSequenceToken: "ddddddd==",
                        $searchScore: 2.5,
                    },
                ]
            },
            vars: {SEARCH_META: {value: 1}},
            ok: 1
        }
    },
    {
        // pagination flag is not set on getMore cursor options
        expectedCommand: {getMore: cursorId, collection: coll.getName()},
        response: {
            cursor: {
                id: NumberLong(0),
                ns: coll.getFullName(),
                nextBatch: [
                    {
                        _id: 5,
                        $searchSequenceToken: "eeeeeee==",
                        $searchScore: 1.234,
                    },
                    {
                        _id: 6,
                        $searchSequenceToken: "fffffff==",
                        $searchScore: 1.345,
                    },
                    {
                        _id: 7,
                        $searchSequenceToken: "ggggggg==",
                        $searchScore: 2.234,
                    },
                    {
                        _id: 8,
                        $searchSequenceToken: "hhhhhhh==",
                        $searchScore: 2.5,
                    },
                ]
            },
            ok: 1
        }
    },
];

assert.commandWorked(
    mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

// Perform a $search query with pagination turned on.
let cursor = coll.aggregate([
    {$search: searchQuery},
    {$project: {"myToken": {$meta: "searchSequenceToken"}, "test": true}}
]);

const expected = [
    {"_id": 1, "test": 100, "myToken": "aaaaaaa=="},
    {"_id": 2, "test": 100, "myToken": "bbbbbbb=="},
    {"_id": 3, "test": 1, "myToken": "ccccccc=="},
    {"_id": 4, "test": 10, "myToken": "ddddddd=="},
    {"_id": 5, "test": 100, "myToken": "eeeeeee=="},
    {"_id": 6, "test": 100, "myToken": "fffffff=="},
    {"_id": 7, "test": 10, "myToken": "ggggggg=="},
    {"_id": 8, "test": 100, "myToken": "hhhhhhh=="}
];
assert.eq(expected, cursor.toArray());

assert.commandWorked(
    mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

// $addFields is $project sugar.
cursor = coll.aggregate(
    [{$search: searchQuery}, {$addFields: {"myToken": {$meta: "searchSequenceToken"}}}]);
assert.eq(expected, cursor.toArray());

const expected2 = [{
    "meta": [{"value": 1}],
    "docs": [
        {"_id": 1, "paginationToken": "aaaaaaa==", "score": 1.234},
        {"_id": 2, "paginationToken": "bbbbbbb==", "score": 1.345},
        {"_id": 3, "paginationToken": "ccccccc==", "score": 2.234},
    ]
}];
assert.commandWorked(
    mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

// Test $search + $facet with searchSequenceToken.
cursor = coll.aggregate([
    {$search: searchQuery},
    {
        $facet: {
            meta: [
                {
                    $replaceWith: "$$SEARCH_META",
                },
                {
                    $limit: 1,
                },
            ],
            docs: [
                {
                    $limit: 3,
                },
                {
                    $project: {
                        paginationToken: {
                            $meta: "searchSequenceToken",
                        },
                        score: {
                            $meta: "searchScore",
                        },
                    },
                },
            ],
        },
    },
]);
assert.eq(expected2, cursor.toArray());

MongoRunner.stopMongod(conn);
mongotmock.stop();
