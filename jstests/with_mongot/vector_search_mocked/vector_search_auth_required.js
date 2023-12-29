/**
 * Test that $vectorSearch command works when mongod and mongot require authentication.
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
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {prepCollection, prepMongotResponse} from "jstests/with_mongot/mongotmock/lib/utils.js";

const dbName = jsTestName();
const collName = "testColl";

const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    limit: 5
};

// Perform a simple vector search command, and test that it succeeds.
function testSimpleVectorSearchQuery(mongodConn, mongotConn) {
    let db = mongodConn.getDB(dbName);
    let coll = db.getCollection(collName);
    const collectionUUID = getUUIDFromListCollections(db, collName);

    const vectorSearchCmd =
        mongotCommandForVectorSearchQuery({collName, collectionUUID, ...vectorSearchQuery, dbName});

    const expected = prepMongotResponse(vectorSearchCmd, coll, mongotConn);

    let cursor = coll.aggregate([{$vectorSearch: vectorSearchQuery}], {cursor: {batchSize: 2}});

    assert.eq(expected, cursor.toArray());
}

// Set up mongotmock. This mongot requires authentication.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConnWithAuth = mongotmock.getConnection();

// Start a mongod normally, and point it at the mongot server.
let mongodConn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConnWithAuth.host}});

// Seed the server with documents.
prepCollection(mongodConn, dbName, collName);

// Test that a vector search query succeeds when mongod and mongot are both configured auth enabled.
testSimpleVectorSearchQuery(mongodConn, mongotConnWithAuth);

// Now test with sharded collection.
MongoRunner.stopMongod(mongodConn);
mongotmock.stop();
let shardingTestOptions = {mongos: 1, shards: {rs0: {nodes: 1}}};

let stWithMock = new ShardingTestWithMongotMock(shardingTestOptions);
stWithMock.start();
let st = stWithMock.st;

let mongos = st.s;
let testDBMongos = mongos.getDB(dbName);
let testCollMongos = testDBMongos.getCollection(collName);

// Create and shard the collection so the commands can succeed.
assert.commandWorked(testDBMongos.createCollection(collName));
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(
    mongos.adminCommand({shardCollection: testCollMongos.getFullName(), key: {a: 1}}));

// Seed the server with documents.
prepCollection(mongos, dbName, collName);

// Test that a vector search query succeeds when auth is configured enabled.
testSimpleVectorSearchQuery(mongos,
                            stWithMock.getMockConnectedToHost(st.rs0.getPrimary()).getConnection());

stWithMock.stop();
