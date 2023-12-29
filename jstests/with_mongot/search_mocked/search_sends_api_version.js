/**
 * Verify that `$search` sends apiVersion if present to mongot.
 * @tags: [
 *   uses_api_parameters,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
const db = conn.getDB(jsTestName());
const coll = db.searchCollector;
coll.drop();
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));

const collUUID = getUUIDFromListCollections(db, coll.getName());
const searchQuery = {
    query: "cakes",
    path: "title"
};

const searchCmd = {
    search: coll.getName(),
    collectionUUID: collUUID,
    query: searchQuery,
    apiVersion: "1",
    $db: jsTestName()
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
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 2, $searchScore: 0.654}, {_id: 1, $searchScore: 0.321}]
                },
                vars: {SEARCH_META: {value: 1}}
            }
        },
    ];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
}

// The command should succeed. We don't care about the results, if the command succeeded mongotmock
// received the apiVersion.
assert.commandWorked(coll.runCommand(
    {aggregate: coll.getName(), apiVersion: "1", pipeline: [{$search: searchQuery}], cursor: {}}));

MongoRunner.stopMongod(conn);
mongotmock.stop();
