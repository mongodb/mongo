/**
 * Test that a mongod running with SSL can connect to a mongotmock (which does not use SSL).
 * @tags: [
 *   requires_fcv_71,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {prepCollection, prepMongotResponse} from "jstests/with_mongot/mongotmock/lib/utils.js";

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

const dbName = "test";
const db = conn.getDB(dbName);
const collName = jsTestName();
const coll = db.getCollection(collName);
prepCollection(conn, dbName, collName);

const collectionUUID = getUUIDFromListCollections(db, collName);
const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    limit: 5
};
const vectorSearchCmd =
    mongotCommandForVectorSearchQuery({...vectorSearchQuery, collName, dbName, collectionUUID});
// Give mongotmock some stuff to return.
const expected = prepMongotResponse(vectorSearchCmd, coll, mongotConn);

// Perform a $vectorSearch query.
let cursor = coll.aggregate([{$vectorSearch: vectorSearchQuery}]);

assert.eq(expected, cursor.toArray());

MongoRunner.stopMongod(conn);
mongotmock.stop();
