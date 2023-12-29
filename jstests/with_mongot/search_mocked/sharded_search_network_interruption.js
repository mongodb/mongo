/**
 * This test enables a failpoint that recreates an interruption on the OpCtx while planShardedSearch
 * is executing. The test assures that the correct error is thrown instead of the server
 * segfaulting.
 */
import {getUUIDFromListCollections} from "jstests/libs/uuid_util.js";
import {
    mockPlanShardedSearchResponse,
    mongotCommandForQuery,
    mongotMultiCursorResponseForBatch,
} from "jstests/with_mongot/mongotmock/lib/mongotmock.js";
import {
    ShardingTestWithMongotMock
} from "jstests/with_mongot/mongotmock/lib/shardingtest_with_mongotmock.js";

const dbName = "test";
const collName = jsTestName();

const stWithMock = new ShardingTestWithMongotMock({
    name: "sharded_search",
    shards: {
        rs0: {nodes: 2},
        rs1: {nodes: 2},
    },
    mongos: 1,
    other: {
        rsOptions: {setParameter: {enableTestCommands: 1}},
    }
});
stWithMock.start();
const st = stWithMock.st;
const mongos = st.s;
const testDB = mongos.getDB(dbName);
const testColl = testDB.getCollection(collName);

assert.commandWorked(
    mongos.getDB("admin").runCommand({enableSharding: dbName, primaryShard: st.shard0.name}));
st.shardColl(testColl, {_id: 1}, {_id: 10}, {_id: 10 + 1});

assert.commandWorked(mongos.getDB("admin").runCommand(
    {configureFailPoint: 'shardedSearchOpCtxDisconnect', mode: 'alwaysOn'}));
const mongotQuery = {
    $search: {}
};

const error = assert.throws(() => testColl.aggregate(mongotQuery));
assert.commandFailedWithCode(error, [11601 /*interrupted*/]);

stWithMock.stop();
