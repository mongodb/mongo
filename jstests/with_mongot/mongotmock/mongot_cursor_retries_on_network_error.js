/**
 * Test the mongot request retry functionality for the $search and $vectorSearch aggregation stages.
 *
 * @tags: [
 *   requires_fcv_71,
 *   # TODO SERVER-99245: re-enable this test once the gRPC error code is correctly reported.
 *   search_community_incompatible,
 * ]
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    MongotMock
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";
import {prepCollection, prepMongotResponse} from "jstests/with_mongot/mongotmock/lib/utils.js";

const dbName = jsTestName();
const collName = jsTestName();

// Set up mongotmock and point the mongod to it.
const mongotmock = new MongotMock();
mongotmock.start();
const mongotConn = mongotmock.getConnection();

const conn = MongoRunner.runMongod({setParameter: {mongotHost: mongotConn.host}});

function runStandaloneTest(stageRegex, pipeline, expectedCommand) {
    prepCollection(conn, dbName, collName);
    const coll = conn.getDB(dbName).getCollection(collName);

    const collectionUUID = getUUIDFromListCollections(conn.getDB(dbName), collName);
    expectedCommand["collectionUUID"] = collectionUUID;
    const expected =
        prepMongotResponse(expectedCommand, coll, mongotConn, NumberLong(123) /* cursorId */);

    // Simulate a case where mongot closes the connection after getting a command.
    // Mongod should retry the command and succeed.
    {
        mongotmock.closeConnectionInResponseToNextNRequests(1);

        let cursor = coll.aggregate(pipeline, {cursor: {batchSize: 2}});
        assert.eq(expected, cursor.toArray());
    }

    // Simulate a case where mongot closes the connection after getting a command,
    // and closes the connection again after receiving the retry.
    // Mongod should only retry once, and the network error from the closed connection should
    // be propogated to the client on retry.
    {
        mongotmock.closeConnectionInResponseToNextNRequests(2);

        const result = assert.throws(() => coll.aggregate(pipeline, {cursor: {batchSize: 2}}));
        assert(isNetworkError(result));
        assert(stageRegex.test(result), `Error wasn't due to stage failing: ${result}`);
    }
}

const searchQuery = {
    query: "cakes",
    path: "title"
};
runStandaloneTest(
    /\$search/, [{$search: searchQuery}], {search: collName, query: searchQuery, $db: dbName});

const vectorSearchQuery = {
    queryVector: [1.0, 2.0, 3.0],
    path: "x",
    numCandidates: 10,
    limit: 5
};
runStandaloneTest(/\$vectorSearch/,
                  [{$vectorSearch: vectorSearchQuery}],
                  {vectorSearch: collName, ...vectorSearchQuery, $db: dbName});

MongoRunner.stopMongod(conn);
mongotmock.stop();

// Now test planShardedSearch (only applicable for $search).
const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 1},
    },
    mongos: 1,
});
stWithMock.start();
let st = stWithMock.st;
let mongos = st.s;
let shardPrimary = st.rs0.getPrimary();
prepCollection(mongos, dbName, collName, true);
const collectionUUID = getUUIDFromListCollections(mongos.getDB(dbName), collName);
const shardedSearchCmd = {
    search: collName,
    collectionUUID: collectionUUID,
    query: searchQuery,
    $db: dbName
};

const mongos_mongotmock = stWithMock.getMockConnectedToHost(mongos);
const shard_mongot_conn = stWithMock.getMockConnectedToHost(shardPrimary).getConnection();

// Single failure
{
    // Mock responses to the planShardedSearch the mongos will issue and the eventual
    // $search command the mongod will issue.
    mockPlanShardedSearchResponse(collName, searchQuery, dbName, undefined, stWithMock);
    const expected = prepMongotResponse(shardedSearchCmd,
                                        shardPrimary.getDB(dbName).getCollection(collName),
                                        shard_mongot_conn,
                                        NumberLong(123) /* cursorId */,
                                        '$searchScore' /* searchScoreKey */);

    // Tell the mongotmock connected to the mongos to close the connection when
    // it receives the initial planShardedSearch from the mongos.
    mongos_mongotmock.closeConnectionInResponseToNextNRequests(1);

    // The mongos should retry the planShardedSearch, allowing the query to succeed.
    let coll = mongos.getDB(dbName).getCollection(collName);
    let cursor = coll.aggregate([{$search: searchQuery}], {cursor: {batchSize: 2}});
    assert.eq(expected, cursor.toArray());
}

// Multiple failures
{
    // Tell the mongotmock connected to the mongos to close the connection when
    // it receives the initial planShardedSearch from the mongos and the retry.
    mongos_mongotmock.closeConnectionInResponseToNextNRequests(2);

    // Error on retry should be propogated out to client.
    let coll = mongos.getDB(dbName).getCollection(collName);
    const result =
        assert.throws(() => coll.aggregate([{$search: searchQuery}], {cursor: {batchSize: 2}}));
    assert(isNetworkError(result));
    assert(/planShardedSearch/.test(result),
           `Error wasn't due to planShardedSearch failing: ${result}`);
}

stWithMock.stop();
