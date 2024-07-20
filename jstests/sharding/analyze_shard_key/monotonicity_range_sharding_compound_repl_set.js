/**
 * Tests that the analyzeShardKey command returns monotonicity metrics for compound shard keys that
 * use range sharding.
 *
 * TODO: SERVER-80318 Remove test
 *
 * @tags: [requires_fcv_70, resource_intensive]
 */
import {
    testAnalyzeShardKeysUnshardedCollection,
} from "jstests/sharding/analyze_shard_key/libs/monotonicity_common.js";
import {numNodesPerRS} from "jstests/sharding/analyze_shard_key/libs/monotonicity_common.js";
import {
    numDocsRange,
    rangeShardingCompoundTestCases,
    testProbability
} from "jstests/sharding/analyze_shard_key/libs/monotonicity_range_sharding_compound_common.js";

if (!jsTestOptions().useAutoBootstrapProcedure) {
    const rst = new ReplSetTest({nodes: numNodesPerRS, oplogSize: 250});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();

    testAnalyzeShardKeysUnshardedCollection(
        primary, rangeShardingCompoundTestCases, testProbability, numDocsRange);

    rst.stopSet();
}
