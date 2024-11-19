/**
 * Tests that the analyzeShardKey command returns correct cardinality and frequency metrics when
 * no document sampling is involved.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_fcv_70,
 * ]
 */
import {
    numMostCommonValues
} from "jstests/sharding/analyze_shard_key/libs/cardinality_and_frequency_common.js";
import {
    testAnalyzeCandidateShardKeysShardedCollection,
    testAnalyzeCandidateShardKeysUnshardedCollection,
    testAnalyzeCurrentShardKeys,
} from "jstests/sharding/analyze_shard_key/libs/cardinality_and_frequency_common_tests.js";

const mongos = db.getMongo();
const shardNames = db.adminCommand({listShards: 1}).shards.map(shard => shard._id);
if (shardNames.length < 2) {
    print(jsTestName() + " will not run; at least 2 shards are required.");
    quit();
}

// Get the number of nodes in a shard's replica set
const shardMap = db.adminCommand({getShardMap: 1});
let numNodesPerRS = 0;
for (const [key, value] of Object.entries(shardMap.map)) {
    if (key !== "config") {
        const nodes = value.split(",").length;
        if (numNodesPerRS == 0) {
            numNodesPerRS = nodes;
        } else {
            assert(nodes >= numNodesPerRS);
        }
    }
}

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues
};
const setParamCmd = Object.assign({setParameter: 1}, setParameterOpts);
assert.commandWorked(db.adminCommand(setParamCmd));

testAnalyzeCandidateShardKeysUnshardedCollection(mongos, {}, writeConcern);
testAnalyzeCandidateShardKeysShardedCollection(mongos, null, writeConcern);
testAnalyzeCurrentShardKeys(mongos, null, writeConcern);
