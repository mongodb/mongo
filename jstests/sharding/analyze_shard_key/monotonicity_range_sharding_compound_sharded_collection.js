/**
 * Tests that the analyzeShardKey command returns monotonicity metrics for compound shard keys that
 * use range sharding.
 *
 * @tags: [requires_fcv_70, resource_intensive]
 */
import {
    testAnalyzeShardKeysShardedCollection,
} from "jstests/sharding/analyze_shard_key/libs/monotonicity_common.js";
import {numNodesPerRS} from "jstests/sharding/analyze_shard_key/libs/monotonicity_common.js";
import {
    numDocsRange,
    rangeShardingCompoundTestCases
} from "jstests/sharding/analyze_shard_key/libs/monotonicity_range_sharding_compound_common.js";

const st = new ShardingTest({shards: 2, rs: {nodes: numNodesPerRS, oplogSize: 500}});

testAnalyzeShardKeysShardedCollection(st, rangeShardingCompoundTestCases, numDocsRange);

st.stop();
