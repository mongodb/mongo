/**
 * Test that moveChunk.error changelog events logged by the MigrationSourceManager have the errmsg field describing the reason for the failure
 *
 * @tags: [
 *   requires_fcv_83
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";

const st = new ShardingTest({shards: 2});

const dbName = "testDB";
const collName = "testColl";
const nss = dbName + "." + collName;
const configDB = st.s.getDB("config");

const primaryShardName = st.shard0.shardName;
const recipientShardName = st.shard1.shardName;
const shardKey = {k: 1};
const splitPoint = {k: 0};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));
st.shardColl(collName, shardKey, splitPoint /*split*/, false /*move*/, dbName);

const donorPrimary = st.rs0.getPrimary();
configureFailPoint(donorPrimary, "failMigrationCommit");

assert.commandFailedWithCode(
    st.s.adminCommand({moveChunk: nss, find: splitPoint, to: recipientShardName}),
    ErrorCodes.InternalError,
);

const expectedErrMsg = "Failing _recvChunkCommit due to failpoint.";
const errorEventCount = configDB.changelog
    .aggregate([
        {
            $match: {
                what: "moveChunk.error",
                "details.errmsg": expectedErrMsg,
            },
        },
    ])
    .itcount();
assert.eq(1, errorEventCount, "Expected to find moveChunk.error event with errmsg: " + expectedErrMsg);

st.stop();
