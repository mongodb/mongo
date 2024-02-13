/**
 * Test the basic case for searchSequenceToken in standalone environment with SBE turned on.
 * @tags: [
 * # Search in SBE requires featureFlagSbeFull to be enabled. Remove this tag when search is not
 * # restricted by featureFlagSbeFull.
 *  featureFlagSbeFull,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod(
    {setParameter: {mongotHost: mongotConn.host, featureFlagSearchInSbe: true}});
const db = conn.getDB("test");
const coll = db.search;
coll.drop();

assert.commandWorked(coll.insert({"_id": 1}));
assert.commandWorked(coll.insert({"_id": 2}));
assert.commandWorked(coll.insert({"_id": 3}));
assert.commandWorked(coll.insert({"_id": 4}));
assert.commandWorked(coll.insert({"_id": 5}));
assert.commandWorked(coll.insert({"_id": 6}));
assert.commandWorked(coll.insert({"_id": 7}));
assert.commandWorked(coll.insert({"_id": 8}));

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
                    {_id: 1, $searchSequenceToken: "aaaaaaa=="},
                    {_id: 2, $searchSequenceToken: "bbbbbbb=="},
                    {_id: 3, $searchSequenceToken: "ccccccc=="},
                    {_id: 4, $searchSequenceToken: "ddddddd=="},
                ]
            },
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
                    {_id: 5, $searchSequenceToken: "eeeeeee=="},
                    {_id: 6, $searchSequenceToken: "fffffff=="},
                    {_id: 7, $searchSequenceToken: "ggggggg=="},
                    {_id: 8, $searchSequenceToken: "hhhhhhh=="},
                ]
            },
            ok: 1
        }
    },
];

assert.commandWorked(
    mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

// Perform a $search query with pagination turned on.
let cursor = coll.aggregate(
    [{$search: searchQuery}, {$project: {"myToken": {$meta: "searchSequenceToken"}}}]);

const expected = [
    {"_id": 1, "myToken": "aaaaaaa=="},
    {"_id": 2, "myToken": "bbbbbbb=="},
    {"_id": 3, "myToken": "ccccccc=="},
    {"_id": 4, "myToken": "ddddddd=="},
    {"_id": 5, "myToken": "eeeeeee=="},
    {"_id": 6, "myToken": "fffffff=="},
    {"_id": 7, "myToken": "ggggggg=="},
    {"_id": 8, "myToken": "hhhhhhh=="}
];
assert.eq(expected, cursor.toArray());

assert.commandWorked(
    mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));

// $addFields is $project sugar.
cursor = coll.aggregate(
    [{$search: searchQuery}, {$addFields: {"myToken": {$meta: "searchSequenceToken"}}}]);
assert.eq(expected, cursor.toArray());

MongoRunner.stopMongod(conn);
mongotmock.stop();
