/**
 * Check that mongod will use the system CA certificates for communication with mongot if
 * tlsUseSystemCA is set.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    CA_CERT,
    SERVER_CERT,
} from "jstests/ssl/libs/ssl_helpers.js";
import {MongotMock} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";

// Set up mongotmock and point the mongod to it, with tlsUseSystemCA enabled.
const mongotmock = new MongotMock();
mongotmock.start({tlsMode: "requireTLS"});
const mongotConn = mongotmock.getConnection();

let env = {};
env["SSL_CERT_FILE"] = CA_CERT;
if (mongotmock.useGRPC()) {
    // gRPC uses a different environment variable to overload the root certificates, so set it here
    // if we are using gRPC.
    env["GRPC_DEFAULT_SSL_ROOTS_FILE_PATH"] = CA_CERT;
}

const conn = MongoRunner.runMongod({
    sslMode: "requireSSL",
    setParameter: {mongotHost: mongotConn.host, tlsUseSystemCA: true, searchTLSMode: "requireTLS"},
    sslPEMKeyFile: SERVER_CERT,
    env,
});

// Now assert that we can perform search queries using the system CA for TLS.
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
