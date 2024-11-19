/**
 * Tests that the analyzeShardKey command returns correct cardinality and frequency metrics when
 * no document sampling is involved.
 *
 * @tags: [requires_fcv_70]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    numMostCommonValues
} from "jstests/sharding/analyze_shard_key/libs/cardinality_and_frequency_common.js";
import {
    testAnalyzeCandidateShardKeysShardedCollection,
    testAnalyzeCandidateShardKeysUnshardedCollection,
    testAnalyzeCurrentShardKeys,
} from "jstests/sharding/analyze_shard_key/libs/cardinality_and_frequency_common_tests.js";

const numNodesPerRS = 2;

// The write concern to use when inserting documents into test collections. Waiting for the
// documents to get replicated to all nodes is necessary since mongos runs the analyzeShardKey
// command with readPreference "secondaryPreferred".
const writeConcern = {
    w: numNodesPerRS
};

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues
};

{
    const st =
        new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS, setParameter: setParameterOpts}});

    testAnalyzeCandidateShardKeysUnshardedCollection(st.s, {st}, writeConcern);
    testAnalyzeCandidateShardKeysShardedCollection(st.s, st, writeConcern);
    testAnalyzeCurrentShardKeys(st.s, st, writeConcern);

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {  // TODO: SERVER-80318 Remove block
    const rst =
        new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();

    testAnalyzeCandidateShardKeysUnshardedCollection(rst.getPrimary(), {rst}, writeConcern);

    rst.stopSet();
}
