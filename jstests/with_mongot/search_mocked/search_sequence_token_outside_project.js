/**
 * Test that $meta: "searchSequenceToken" can be referenced in non-project stages where meta is
 * allowed.
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

};
const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: "test",
    cursorOptions: {requiresSearchSequenceToken: true}
};

// Give mongotmock some stuff to return.
let x = new BinData(0, "1234");
const cursorId = NumberLong(123);
const history = [
    {
        expectedCommand: searchCmd,
        response: {
            cursor: {
                id: NumberLong(0),
                ns: coll.getFullName(),
                nextBatch: [
                    {_id: 1, $searchSequenceToken: "aaaaaaa=="},
                    {_id: 2, $searchSequenceToken: "bbbbbbb=="},
                    {_id: 3, $searchSequenceToken: "ccccccc=="},
                    {_id: 4, $searchSequenceToken: "ddddddd=="},
                    {_id: 5, $searchSequenceToken: "eeeeeee=="},
                    {_id: 6, $searchSequenceToken: "fffffff=="},
                    {_id: 7, $searchSequenceToken: "ggggggg=="},
                    {_id: 8, $searchSequenceToken: "hhhhhhh=="},
                ]
            },
        }
    },

];

assert.commandWorked(
    mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
let results =
    coll.aggregate([{$search: searchQuery}, {$group: {"_id": {$meta: "searchSequenceToken"}}}])
        .toArray();

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
// base64 does not preserve sort order of unencoded strings, so the $group query will have
// nondeterministic results.
assert.eq(expected.length, results.length);
results.every((element_1) => expected.some((element_2) => element_1._id === element_2._id));

MongoRunner.stopMongod(conn);
mongotmock.stop();
