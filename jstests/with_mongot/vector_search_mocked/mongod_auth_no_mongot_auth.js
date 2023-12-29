/**
 * Test that when mongod is configured with auth enabled, but mongot does not have auth
 * enabled, mongod can still connect to mongot so long as the skipAuthenticationToMongot server
 * parameter is set on mongod.
 *
 * @tags: [
 *   requires_fcv_71,
 *   requires_auth,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mongotCommandForVectorSearchQuery,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {prepCollection, prepMongotResponse} from "jstests/with_mongot/mongotmock/lib/utils.js";

// Give mongotmock some stuff to return.

function makeVectorSearchCmd(db, coll, vectorSearchQuery) {
    const collUUID = getUUIDFromListCollections(db, coll.getName());
    const vectorSearchCmd = mongotCommandForVectorSearchQuery(
        {collName: coll.getName(), collectionUUID: collUUID, ...vectorSearchQuery, dbName: "test"});
    return vectorSearchCmd;
}

const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    index: "index",
    limit: 5
};

// Set up mongotmock and point the mongod to it. Configure the mongotmock to not have auth.
const mongotmock = new MongotMock();
mongotmock.start({bypassAuth: true});
const mongotConn = mongotmock.getConnection();

// Start a mongod normally, and point it at the mongot.
let conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});
let db = conn.getDB("test");
const collName = jsTestName();
let coll = db.getCollection(collName);

// Insert some docs.
prepCollection(conn, "test", collName);

// Perform a $vectorSearch query. We expect the command to fail because mongod will attempt
// to authenticate with mongot, which will not support authentication.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$vectorSearch: vectorSearchQuery}],
    cursor: {batchSize: 2}
}),
                             ErrorCodes.AuthenticationFailed);

// Restart mongod with the option to not authenticate mongod <--> mongot connections.
MongoRunner.stopMongod(conn);
conn = MongoRunner.runMongod(
    {setParameter: {mongotHost: mongotConn.host, skipAuthenticationToMongot: true}});
db = conn.getDB("test");
coll = db.getCollection(collName);

// Insert some docs.
prepCollection(conn, "test", collName);

const expected =
    prepMongotResponse(makeVectorSearchCmd(db, coll, vectorSearchQuery), coll, mongotConn);

// Perform a vector search query. We expect the command to succeed, demonstrating that
// mongod <--> mongot communication is up.
let cursor = coll.aggregate([{$vectorSearch: vectorSearchQuery}], {cursor: {batchSize: 2}});

assert.eq(expected, cursor.toArray());

MongoRunner.stopMongod(conn);
mongotmock.stop();
