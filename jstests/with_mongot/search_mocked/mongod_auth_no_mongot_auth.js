/**
 * Test that when mongod is configured with auth enabled, but mongot does not have auth
 * enabled, mongod can still connect to mongot so long as the skipAuthenticationToMongot server
 * parameter is set on mongod.
 *
 * @tags: [requires_auth]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

function insertDataForSearch(coll) {
    assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
    assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
    assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));
    assert.commandWorked(coll.insert({"_id": 4, "title": "oranges"}));
    assert.commandWorked(coll.insert({"_id": 5, "title": "cakes and oranges"}));
    assert.commandWorked(coll.insert({"_id": 6, "title": "cakes and apples"}));
    assert.commandWorked(coll.insert({"_id": 7, "title": "apples"}));
    assert.commandWorked(coll.insert({"_id": 8, "title": "cakes and kale"}));
}

// Give mongotmock some stuff to return.
function prepareMongotResponse(searchCmd, coll) {
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

function makeSearchCmd(db, coll, searchQuery) {
    const collUUID = getUUIDFromListCollections(db, coll.getName());
    const searchCmd =
        {search: coll.getName(), collectionUUID: collUUID, query: searchQuery, $db: "test"};
    return searchCmd;
}

const searchQuery = {
    query: "cakes",
    path: "title"
};

// Set up mongotmock and point the mongod to it. Configure the mongotmock to not have auth.
const mongotmock = new MongotMock();
mongotmock.start({bypassAuth: true});
const mongotConn = mongotmock.getConnection();

// Start a mongod normally, and point it at the mongot.
let conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
let db = conn.getDB("test");
let coll = db.search;
coll.drop();

// Insert some docs.
insertDataForSearch(coll);

// Perform a $search query. We expect the command to fail because mongot will attempt
// to authenticate with mongot, which will not support authentication.
assert.commandFailedWithCode(
    db.runCommand(
        {aggregate: coll.getName(), pipeline: [{$search: searchQuery}], cursor: {batchSize: 2}}),
    ErrorCodes.AuthenticationFailed);

// Restart mongod with the option to not authenticate mongod <--> mongot connections.
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod(
    {setParameter: {mongotHost: mongotConn.host, skipAuthenticationToMongot: true}});
db = conn.getDB("test");
coll = db.search;
coll.drop();

// Insert some docs.
insertDataForSearch(coll);

prepareMongotResponse(makeSearchCmd(db, coll, searchQuery), coll);

// Perform a search query. We expect the command to succeed, demonstrating that
// mongod <--> mongot communication is up.
let cursor = coll.aggregate([{$search: searchQuery}], {cursor: {batchSize: 2}});

const expected = [
    {"_id": 1, "title": "cakes"},
    {"_id": 2, "title": "cookies and cakes"},
    {"_id": 5, "title": "cakes and oranges"},
    {"_id": 6, "title": "cakes and apples"},
    {"_id": 8, "title": "cakes and kale"}
];

assert.eq(expected, cursor.toArray());

MongoRunner.stopMongod(conn);
mongotmock.stop();
