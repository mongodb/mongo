/**
 * Verifies that mapReduce does not replace its output again after its shard is removed.
 * @tags: [
 *   requires_scripting,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Skip the teardown metadata check: it waits for replication on the shard we stopped.
TestData.skipCheckShardFilteringMetadata = true;

const st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 1}});
const dbName = jsTestName();
const testDB = st.s.getDB(dbName);
const retiringShard = st.shard1;
assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: retiringShard.shardName}),
);
assert.commandWorked(testDB.input.insert({_id: 1, x: 10}));

const afterExecution = configureFailPoint(
    st.rs1.getPrimary(),
    "waitAfterCommandFinishesExecution",
    {comment: dbName, commands: ["aggregate"], shouldCheckForInterrupt: true},
);
const worker = new Thread(
    (host, dbName) => {
        return new Mongo(host).getDB(dbName).runCommand({
            mapReduce: "input",
            map: function () {
                emit("a", this.x);
            },
            reduce: function (key, values) {
                return Array.sum(values);
            },
            out: "output",
            comment: dbName,
            writeConcern: {w: "majority"},
        });
    },
    st.s.host,
    dbName,
);
worker.start();
afterExecution.wait({maxTimeMS: 60_000});

assert.eq(10, retiringShard.getDB(dbName).output.findOne({_id: "a"}).value);
// The first run wrote 10. Change the input so a second run would replace it with 20.
assert.commandWorked(testDB.input.update({_id: 1}, {$set: {x: 20}}));

assert.commandWorked(st.s.adminCommand({startShardDraining: retiringShard.shardName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));
assert.soon(
    () =>
        assert.commandWorked(st.s.adminCommand({shardDrainingStatus: retiringShard.shardName}))
            .state === "drainingComplete",
    "The retiring shard did not finish draining",
    60_000,
);
assert.commandWorked(st.s.adminCommand({commitShardRemoval: retiringShard.shardName}));

// Stop the shard before it can send its successful reply to mongos.
st.rs1.stopSet(undefined, false, {skipValidation: true, skipCheckDBHashes: true});
const result = worker.returnData();
assert.eq(10, testDB.output.findOne({_id: "a"}).value, "MapReduce must not run a second time");
assert.commandFailedWithCode(result, ErrorCodes.QueryPlanKilled);

st.stop();
