/**
 * Verifies that shard removal does not cause an already-applied $merge update to be applied twice.
 * The whenMatched pipeline adds 10 to an existing total. If mongos retries the completed
 * aggregation after losing its response, the total becomes 20 instead of the expected 10.
 *
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// This test leaves a shard stopped, so the filtering-metadata hook cannot await replication.
TestData.skipCheckShardFilteringMetadata = true;

const st = new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 1}});
const dbName = jsTestName();
const retiringShard = st.shard1;
const survivingShard = st.shard0;
const db = st.s.getDB(dbName);
const source = db.source;
const target = db.target;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: retiringShard.shardName}),
);
assert.commandWorked(source.insert({_id: 1}));
assert.commandWorked(target.insert({_id: 1, total: 0}));

const afterExecution = configureFailPoint(
    st.rs1.getPrimary(),
    "waitAfterCommandFinishesExecution",
    {comment: dbName, commands: ["aggregate"], shouldCheckForInterrupt: true},
);

const aggregateThread = new Thread(
    (host, dbName) => {
        const conn = new Mongo(host);
        return conn.getDB(dbName).runCommand({
            aggregate: "source",
            pipeline: [
                {
                    $merge: {
                        into: "target",
                        whenMatched: [{$set: {total: {$add: ["$total", 10]}}}],
                    },
                },
            ],
            cursor: {},
            comment: dbName,
            writeConcern: {w: "majority"},
        });
    },
    st.s.host,
    dbName,
);
aggregateThread.start();

jsTest.log.info("Waiting for $merge's first update to apply before its response is released");
afterExecution.wait({maxTimeMS: 60_000});

const afterFirstExecution = retiringShard.getDB(dbName).target.findOne({_id: 1});
assert.eq(10, afterFirstExecution.total, "The first $merge execution should add 10");

assert.commandWorked(st.s.adminCommand({startShardDraining: retiringShard.shardName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: survivingShard.shardName}));
assert.soon(
    () =>
        assert.commandWorked(st.s.adminCommand({shardDrainingStatus: retiringShard.shardName}))
            .state === "drainingComplete",
    "The retiring shard did not finish draining",
    60_000,
);
assert.commandWorked(st.s.adminCommand({commitShardRemoval: retiringShard.shardName}));

// Stop the shard with its successful reply still held. Mongos must report an error rather than
// retrying the aggregation against the surviving shard.
st.rs1.stopSet(undefined, false, {skipValidation: true, skipCheckDBHashes: true});

const aggregateResult = aggregateThread.returnData();
jsTest.log.info("$merge result after shard removal", {aggregateResult});
assert.commandFailedWithCode(aggregateResult, ErrorCodes.QueryPlanKilled);

const finalTargetDoc = target.findOne({_id: 1});
jsTest.log.info("Target document after shard removal", {afterFirstExecution, finalTargetDoc});
assert.eq(
    10,
    finalTargetDoc.total,
    "The same $merge update must not be applied twice after shard removal",
    {afterFirstExecution, finalTargetDoc},
);

st.stop();
