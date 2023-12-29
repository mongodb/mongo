/**
 * Test that authentications between mongod and the search index management server can skip auth
 * when appropriately configured, even when connections between mongod and mongot require
 * authentication.
 *
 * @tags: [requires_auth]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = jsTestName();
const collName = "testColl";

const searchQuery = {
    query: "cakes",
    path: "title"
};

// Perform a simple search-query command, and test that it succeeds.
function testSimpleSearchQuery(mongodConn, mongotConn) {
    let db = mongodConn.getDB(dbName);
    let coll = db.getCollection(collName);
    const collUUID = getUUIDFromListCollections(db, collName);

    const searchCmd =
        {search: coll.getName(), collectionUUID: collUUID, query: searchQuery, $db: dbName};

    const history = [
        {
            expectedCommand: searchCmd,
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [{_id: 1, $searchScore: 0.321}]
                },
                ok: 1
            }
        },
    ];

    assert.commandWorked(mongotConn.adminCommand(
        {setMockResponses: 1, cursorId: NumberLong(123), history: history}));

    let cursor = coll.aggregate([{$search: searchQuery}], {cursor: {batchSize: 2}});
    const expected = [{"_id": 1, "title": "cakes"}];

    assert.eq(expected, cursor.toArray());
}

function assertCreateSearchIndexFailsAuth(conn) {
    let testDB = conn.getDB(dbName);
    assert.commandFailedWithCode(testDB.runCommand({
        'createSearchIndexes': collName,
        'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
    }),
                                 ErrorCodes.AuthenticationFailed);
}

function assertCreateSearchIndexSucceeds(mongosConn, searchIndexServerMock) {
    const manageSearchIndexCommandResponse = {
        indexesCreated: [{id: "index-Id", name: "index-name"}]
    };

    searchIndexServerMock.setMockSearchIndexCommandResponse(manageSearchIndexCommandResponse);
    let testDB = mongosConn.getDB(dbName);
    assert.commandWorked(testDB.runCommand({
        'createSearchIndexes': collName,
        'indexes': [{'definition': {'mappings': {'dynamic': true}}}]
    }));
}

// Set up mongotmock. This mongot won't skip authentication.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConnWithAuth = mongotmock.getConnection();

// Setup mock process that mocks the search-index-management server.
// Currently, the search index management server does not support authentication. Therefore, for
// testing, we start the mock process with authentication disabled to properly simulate it.
const searchIndexServerMock = new MongotMock();
searchIndexServerMock.start({bypassAuth: true});
const searchIndexServerConn = searchIndexServerMock.getConnection();

// Start a mongod normally, and point it at the mongot and search index server.
// Set the mongod to use authentication with the search index server.
let mongodConn = MongoRunner.runMongod({
    setParameter: {
        mongotHost: mongotConnWithAuth.host,
        searchIndexManagementHostAndPort: searchIndexServerConn.host,
        skipAuthenticationToSearchIndexManagementServer: false
    }
});

// Seed the server with a document.
let coll = mongodConn.getDB(dbName).getCollection(collName);
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

// Perform a search-index-management command. We expect the command to fail because mongod will
// attempt to authenticate with the search-index-server, which will not support authentication.
assertCreateSearchIndexFailsAuth(mongodConn);

// Test that a search query to mongot still succeeds, since both mongod and mongot support auth in
// this configuration.
testSimpleSearchQuery(mongodConn, mongotConnWithAuth);

// Restart mongod with the option set to skip authentication for mongod <--> search-index-server
// connections. This will allow successful commands between servers because the search index
// management server cannot respond to authentication requests/commands from mongod.
MongoRunner.stopMongod(mongodConn);
mongodConn = MongoRunner.runMongod({
    setParameter: {
        mongotHost: mongotConnWithAuth.host,
        searchIndexManagementHostAndPort: searchIndexServerConn.host,
        skipAuthenticationToSearchIndexManagementServer: true
    }
});

// Seed the server with a document.
coll = mongodConn.getDB(dbName).getCollection(collName);
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

assertCreateSearchIndexSucceeds(mongodConn, searchIndexServerMock);

// Search queries should still work fine.
testSimpleSearchQuery(mongodConn, mongotConnWithAuth);
MongoRunner.stopMongod(mongodConn);

// Now test that mongos <--> search index management server authentication is configurable as well.
// We don't need a manually-managed mongotmock anymore because ShardingTestWithMongotMock will
// provide one for queries.
mongotmock.stop();
let shardingTestOptions = {
    mongos: 1,
    shards: {rs0: {nodes: 1}},
    other: {
        mongosOptions: {
            setParameter: {
                searchIndexManagementHostAndPort: searchIndexServerConn.host,
                skipAuthenticationToSearchIndexManagementServer: false
            }
        },
        rs0: {
            setParameter: {
                searchIndexManagementHostAndPort: searchIndexServerConn.host,
            },
        },
    }
};

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

// Seed the server with a document.
assert.commandWorked(testCollMongos.insert({"_id": 1, "title": "cakes"}));

// Perform a search-index-management command. We expect the command to fail because mongos will
// attempt to authenticate with the search-index-server, which will not support authentication.
assertCreateSearchIndexFailsAuth(mongos);

// Test that a search query to mongot still succeeds, since both mongod and mongot support auth in
// this configuration. Set up the mongos with a mocked planning response first.
mockPlanShardedSearchResponse(collName, searchQuery, dbName, undefined /*sortSpec*/, stWithMock);
testSimpleSearchQuery(mongos,
                      stWithMock.getMockConnectedToHost(st.rs0.getPrimary()).getConnection());

stWithMock.stop();

// Restart mongos with the option set to turn off authentication for mongos <--> search-index-server
// connections.
shardingTestOptions.other.mongosOptions.setParameter
    .skipAuthenticationToSearchIndexManagementServer = true;
stWithMock = new ShardingTestWithMongotMock(shardingTestOptions);
stWithMock.start();
st = stWithMock.st;

mongos = st.s;
testDBMongos = mongos.getDB(dbName);
testCollMongos = testDBMongos.getCollection(collName);

// Create and shard the collection so the commands can succeed.
assert.commandWorked(testDBMongos.createCollection(collName));
assert.commandWorked(mongos.adminCommand({enableSharding: dbName}));
assert.commandWorked(
    mongos.adminCommand({shardCollection: testCollMongos.getFullName(), key: {a: 1}}));

// Seed the server with a document.
assert.commandWorked(testCollMongos.insert({"_id": 1, "title": "cakes"}));

// We now expect the index management command to succeed because both are not using authentication.
assertCreateSearchIndexSucceeds(mongos, searchIndexServerMock);

// Search queries should still work fine.
// Set up the mongos with a mocked planning response first.
mockPlanShardedSearchResponse(collName, searchQuery, dbName, undefined /*sortSpec*/, stWithMock);
testSimpleSearchQuery(mongos,
                      stWithMock.getMockConnectedToHost(st.rs0.getPrimary()).getConnection());

stWithMock.stop();
searchIndexServerMock.stop();
