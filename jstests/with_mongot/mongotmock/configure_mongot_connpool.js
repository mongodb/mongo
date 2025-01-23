/**
 * @tags: [
 *  requires_replication,
 *  requires_sharding,
 *  sets_replica_set_matching_strategy,
 *   # TODO SERVER-99246: re-enable this test once the connection pool stats with gRPC are correct.
 *   search_community_incompatible,
 * ]
 */

import {assertHasConnPoolStats} from "jstests/libs/conn_pool_helpers.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

let cursorIdCounter = 1;
let currentCheckNum = 0;

const searchQuery = {
    query: "cakes",
    path: "title"
};

function updateSetParameters(conn, params) {
    let cmd = Object.assign({"setParameter": 1}, params);
    assert.commandWorked(conn.adminCommand(cmd));
}

function launchSearchQuery(mongod, threads, {times}) {
    jsTestLog("Starting " + times + " connections");
    for (let i = 0; i < times; i++) {
        let thread = new Thread(function(connStr, dbName, collName, searchQuery) {
            let client = new Mongo(connStr);
            let db = client.getDB(dbName);
            assert.commandWorked(db.runCommand(
                {aggregate: collName, pipeline: [{$search: searchQuery}], cursor: {}}));
        }, mongod.host, 'test', 'search', searchQuery);
        thread.start();
        threads.push(thread);
    }
}

function prepSearchResponses(mongotConn, howMany, coll, collUUID, stWithMock = undefined) {
    for (let i = 0; i < howMany; i++) {
        let history = [];
        let singleResponse = {
            expectedCommand: mongotCommandForQuery({
                query: searchQuery,
                collName: coll.getName(),
                db: coll.getDB().getName(),
                collectionUUID: collUUID
            }),
            response: {
                cursor: {
                    id: NumberLong(0),
                    ns: coll.getFullName(),
                    nextBatch: [
                        {_id: 1, $searchScore: 0.321},
                        {_id: 2, $searchScore: 0.654},
                        {_id: 5, $searchScore: 0.789}
                    ]
                },
                ok: 1
            }
        };
        history.push(singleResponse);
        if (stWithMock) {
            mockPlanShardedSearchResponse(coll.getName(),
                                          searchQuery,
                                          coll.getDB().getName(),
                                          undefined /*sortSpec*/,
                                          stWithMock);
        }
        assert.commandWorked(mongotConn.adminCommand(
            {setMockResponses: 1, cursorId: NumberLong(++cursorIdCounter), history: history}));
    }
}

const kPoolMinSize = 4;
const kPoolMaxSize = 8;

// Test that for a mongo{d,s} server at `conn` min/max pool sizes between the server and the mongot
// mocked by mongotconn and/or the mocks of stWithMock are respected. mongotConn should be a
// connection to the mongotmock that responds to mongod's data-queries; stWithMock should contain
// all the mongotmocks for a sharded deployment.
function testMinAndMax(conn, mongotConn, stWithMock = undefined) {
    // If we're testing mongos <--> mongot conn pool, we need to block the mongotmock that responds
    // to mongos rather than the mongotmock that responds to mongot's queries for actual data.
    let threads = [];
    let mongotMockToBlock =
        stWithMock ? stWithMock.getMockConnectedToHost(mongos).getConnection() : mongotConn;
    assert.commandWorked(
        conn.adminCommand({_dropConnectionsToMongot: 1, hostAndPort: [mongotMockToBlock.host]}));

    let db = conn.getDB("test");
    let coll = db.search;
    if (stWithMock) {
        assert.commandWorked(db.createCollection("search"));
        assert.commandWorked(conn.adminCommand({enableSharding: "test"}));
        assert.commandWorked(conn.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));
    }
    assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));
    assert.commandWorked(coll.insert({"_id": 2, "title": "cookies and cakes"}));
    assert.commandWorked(coll.insert({"_id": 3, "title": "vegetables"}));
    let collUUID = getUUIDFromListCollections(coll.getDB(), coll.getName());

    // Launch an initial search query to trigger to min.
    prepSearchResponses(mongotConn, 1, coll, collUUID, stWithMock);
    launchSearchQuery(conn, threads, {times: 1});
    currentCheckNum = assertHasConnPoolStats(conn,
                                             [mongotMockToBlock.host],
                                             {ready: kPoolMinSize},
                                             currentCheckNum,
                                             "_mongotConnPoolStats");

    // Increase by one.
    const newMinSize = 5;
    updateSetParameters(conn, {mongotConnectionPoolMinSize: newMinSize});
    currentCheckNum = assertHasConnPoolStats(conn,
                                             [mongotMockToBlock.host],
                                             {ready: newMinSize},
                                             currentCheckNum,
                                             "_mongotConnPoolStats");

    // Increase to maxSize
    updateSetParameters(conn, {mongotConnectionPoolMinSize: kPoolMaxSize});
    currentCheckNum = assertHasConnPoolStats(conn,
                                             [mongotMockToBlock.host],
                                             {ready: kPoolMaxSize},
                                             currentCheckNum,
                                             "_mongotConnPoolStats");

    // Ensure the search query completes successfully.
    for (let i = 0; i < threads.length; i++) {
        threads[i].join();
    }

    // Reset the pool to mongot before testing max.
    updateSetParameters(conn, {mongotConnectionPoolMinSize: 0});
    assert.commandWorked(
        conn.adminCommand({_dropConnectionsToMongot: 1, hostAndPort: [mongotMockToBlock.host]}));
    currentCheckNum = assertHasConnPoolStats(
        conn, [mongotMockToBlock.host], {isAbsent: true}, currentCheckNum, "_mongotConnPoolStats");

    // Launch kPoolMaxSize - 1 blocked finds.
    threads = [];
    assert.commandWorked(mongotMockToBlock.adminCommand(
        {configureFailPoint: "mongotWaitBeforeRespondingToQuery", mode: 'alwaysOn'}));
    prepSearchResponses(mongotConn, kPoolMaxSize - 1, coll, collUUID, stWithMock);
    launchSearchQuery(conn, threads, {times: kPoolMaxSize - 1});
    currentCheckNum = assertHasConnPoolStats(conn,
                                             [mongotMockToBlock.host],
                                             {active: kPoolMaxSize - 1},
                                             currentCheckNum,
                                             "_mongotConnPoolStats");

    // Launch two more.
    prepSearchResponses(mongotConn, 2, coll, collUUID, stWithMock);
    launchSearchQuery(conn, threads, {times: 2});
    // We should only go up to the max of active connections.
    currentCheckNum = assertHasConnPoolStats(conn,
                                             [mongotMockToBlock.host],
                                             {active: kPoolMaxSize},
                                             currentCheckNum,
                                             "_mongotConnPoolStats");

    // Increase the max size, which will allow us to have as many active connections as there are
    // waiting requests.
    updateSetParameters(conn, {mongotConnectionPoolMaxSize: kPoolMaxSize + 1});
    currentCheckNum = assertHasConnPoolStats(conn,
                                             [mongotMockToBlock.host],
                                             {active: kPoolMaxSize + 1},
                                             currentCheckNum,
                                             "_mongotConnPoolStats");

    // Release the search queries and assert we have the max number of connections ready.
    assert.commandWorked(mongotMockToBlock.adminCommand(
        {configureFailPoint: "mongotWaitBeforeRespondingToQuery", mode: 'off'}));
    currentCheckNum = assertHasConnPoolStats(conn,
                                             [mongotMockToBlock.host],
                                             {pending: 0, ready: kPoolMaxSize + 1},
                                             currentCheckNum,
                                             "_mongotConnPoolStats");

    // Ensure the search queries complete successfully.
    for (let i = 0; i < threads.length; i++) {
        threads[i].join();
    }
}

const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

// Start a mongod normally, and point it at the mongot. Configure the mongod <--> mongod connection
// pool to have appropriate size limits.
let conn = MongoRunner.runMongod({
    setParameter: {
        mongotHost: mongotConn.host,
        mongotConnectionPoolMinSize: kPoolMinSize,
        mongotConnectionPoolMaxSize: kPoolMaxSize,
    }
});

testMinAndMax(conn, mongotConn);
MongoRunner.stopMongod(conn);
mongotmock.stop();

// Start a sharded cluster, and ensure the mongos <--> mongot connection pool respects the limits as
// well.
let shardingTestOptions = {
    mongos: 1,
    shards: {rs0: {nodes: 1}},
    other: {
        mongosOptions: {
            setParameter: {
                mongotConnectionPoolMinSize: kPoolMinSize,
                mongotConnectionPoolMaxSize: kPoolMaxSize,
            }
        },
    }
};

let stWithMock = new ShardingTestWithMongotMock(shardingTestOptions);
stWithMock.start();
let st = stWithMock.st;

let mongos = st.s;

testMinAndMax(
    mongos, stWithMock.getMockConnectedToHost(st.rs0.getPrimary()).getConnection(), stWithMock);

stWithMock.stop();
