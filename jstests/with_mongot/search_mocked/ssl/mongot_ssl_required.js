/**
 * Test that a mongod running with SSL can connect to a mongotmock (which does not use SSL).
 *
 * @tags: [
 *  # The ingress gRPC server on mongotmock requires that TLS is enabled.
 *  search_community_incompatible
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({
    sslMode: "requireSSL",
    sslPEMKeyFile: "jstests/libs/password_protected.pem",
    sslPEMKeyPassword: "qwerty",
    setParameter: {mongotHost: mongotConn.host, searchTLSMode: "disabled"},
    sslCAFile: "jstests/libs/ca.pem"
});

const db = conn.getDB("test");
const collName = "search";
db[collName].drop();
assert.commandWorked(db[collName].insert({"_id": 1, "title": "cakes"}));

const collUUID = getUUIDFromListCollections(db, collName);
const searchQuery = {
    query: "cakes",
    path: "title"
};

// Give mongotmock some stuff to return.
{
    const cursorId = NumberLong(123);
    const searchCmd = {search: collName, collectionUUID: collUUID, query: searchQuery, $db: "test"};
    const history = [
        {
            expectedCommand: searchCmd,
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: "test." + collName,
                    nextBatch: [{_id: 1, $searchScore: 0.321}]
                },
                ok: 1
            }
        },
    ];

    assert.commandWorked(
        mongotConn.adminCommand({setMockResponses: 1, cursorId: cursorId, history: history}));
}

// Perform a $search query.
let cursor = db[collName].aggregate([{$search: searchQuery}]);

const expected = [
    {"_id": 1, "title": "cakes"},
];
assert.eq(expected, cursor.toArray());

MongoRunner.stopMongod(conn);
mongotmock.stop();
