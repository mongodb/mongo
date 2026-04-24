/**
 * Tests that the analyzeShardKey command returns correct cardinality and frequency metrics when
 * no document sampling is involved.
 *
 * @tags: [requires_fcv_70, requires_profiling]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {AnalyzeShardKeyUtil} from "jstests/sharding/analyze_shard_key/libs/analyze_shard_key_util.js";
import {numMostCommonValues} from "jstests/sharding/analyze_shard_key/libs/cardinality_and_frequency_common.js";
import {
    testAnalyzeCandidateShardKeysShardedCollection,
    testAnalyzeCandidateShardKeysUnshardedCollection,
    testAnalyzeCurrentShardKeys,
} from "jstests/sharding/analyze_shard_key/libs/cardinality_and_frequency_common_tests.js";

const numNodesPerRS = 2;

const setParameterOpts = {
    analyzeShardKeyNumMostCommonValues: numMostCommonValues,
};

{
    const st = new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS, setParameter: setParameterOpts}});

    const writeConcern = AnalyzeShardKeyUtil.getReplicationWriteConcern(st.s, numNodesPerRS);
    testAnalyzeCandidateShardKeysUnshardedCollection(st.s, {st}, writeConcern);
    testAnalyzeCandidateShardKeysShardedCollection(st.s, st, writeConcern);
    testAnalyzeCurrentShardKeys(st.s, st, writeConcern);

    st.stop();
}

if (!jsTestOptions().useAutoBootstrapProcedure) {
    // TODO: SERVER-80318 Remove block
    const rst = new ReplSetTest({nodes: numNodesPerRS, nodeOptions: {setParameter: setParameterOpts}});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const writeConcern = AnalyzeShardKeyUtil.getReplicationWriteConcern(primary, numNodesPerRS);
    testAnalyzeCandidateShardKeysUnshardedCollection(primary, {rst}, writeConcern);

    rst.stopSet();
}
