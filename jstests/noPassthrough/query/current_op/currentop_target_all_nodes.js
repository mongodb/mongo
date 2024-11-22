// Tests that the $currentOp works as expected when run with the targetAllNodes option turned on and
// off.
//
// @tags: [
// requires_fcv_71,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";

const shardCount = 2;
const rsNodesPerShardCount = 2;
const st = new ShardingTest({shards: shardCount, rs: {nodes: rsNodesPerShardCount}});
const clusterAdminDB = st.s.getDB("admin");

function runCurrentOpAgg(shouldTargetAllNodes) {
    return clusterAdminDB.aggregate(
        [
            {$currentOp: {targetAllNodes: shouldTargetAllNodes}},
            {$match: {"command.comment": "issuing a currentOp with targetAllNodes"}}
        ],
        {comment: "issuing a currentOp with targetAllNodes"});
}

const targetAllNodesFalse = runCurrentOpAgg(false);
assert.eq(shardCount, targetAllNodesFalse.itcount(), tojson(targetAllNodesFalse));

const targetAllNodesTrue = runCurrentOpAgg(true);
assert.eq(
    shardCount * rsNodesPerShardCount, targetAllNodesTrue.itcount(), tojson(targetAllNodesTrue));

st.stop();