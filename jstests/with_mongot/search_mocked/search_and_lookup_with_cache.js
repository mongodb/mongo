/**
 * Tests that a pipeline with search followed by lookup can correctly return results when planned
 * from the cache.
 */

import {checkSbeCompletelyDisabled, checkSbeRestricted} from "jstests/libs/sbe_util.js";
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
db.search2.insertOne({name: "foo"});

assert.commandWorked(coll.insert([
    {'_id': ObjectId('65c2cef405d33a7aa3a1cd7a'), 'name': 'apple jam'},
    {'_id': ObjectId('65c2cef405d33a7aa3a1cd7c'), 'name': 'apple blueberry jam'}
]));

const collUUID = getUUIDFromListCollections(db, coll.getName());
const searchQuery = {
    path: "name",
    query: "apple"
};
const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    $db: "test"
};

// Give mongotmock some docs to return.
function prepareForSearch() {
    const cursorId = NumberLong(0);
    const history = [
        {
            expectedCommand: searchCmd,
            response: {
                cursor: {
                    id: cursorId,
                    ns: coll.getFullName(),
                    nextBatch: [
                        {
                            '_id': ObjectId('65c2cef405d33a7aa3a1cd7a'),
                            'name': 'apple jam',
                            '$searchScore': 0.22689829766750336,
                        },
                        {
                            '_id': ObjectId('65c2cef405d33a7aa3a1cd7c'),
                            'name': 'apple blueberry jam',
                            '$searchScore': 0.1912805438041687,
                        }
                    ]
                },
                vars: {'SEARCH_META': {'count': {'lowerBound': 2}}},
                ok: 1
            }
        },
    ];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: NumberLong(1), history: history}));
}
prepareForSearch();

// Perform a $search query.
const pipeline = [
    {$search: searchQuery},
    {$addFields: {score: {$meta: "searchScore"}, searchMeta: "$$SEARCH_META"}},
    {$lookup: {from: "search2", localField: "name", foreignField: "name", as: "match"}}
];
let results = coll.aggregate(pipeline).toArray();

const expected = [
    {
        '_id': ObjectId('65c2cef405d33a7aa3a1cd7a'),
        'name': 'apple jam',
        'score': 0.22689829766750336,
        'searchMeta': {'count': {'lowerBound': 2}},
        match: []
    },
    {
        '_id': ObjectId('65c2cef405d33a7aa3a1cd7c'),
        'name': 'apple blueberry jam',
        'score': 0.1912805438041687,
        'searchMeta': {'count': {'lowerBound': 2}},
        match: []
    }
];
assert.eq(expected, results);

prepareForSearch();

// It is important to run it twice to stress the plan caching logic.
results = coll.aggregate(pipeline).toArray();
assert.eq(expected, results);

MongoRunner.stopMongod(conn);
mongotmock.stop();
