/**
 * Tests that DISTINCT_SCAN queries against a sharded collection don't apply shard filtering when
 * the read concern level is "available".
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan
 * ]
 */

import {
    note,
    outputAggregationPlanAndResults,
    outputDistinctPlanAndResults,
    section,
    subSection
} from "jstests/libs/pretty_md.js";
import {setupShardedCollectionWithOrphans} from "jstests/libs/query_golden_sharding_utils.js";

TestData.skipCheckOrphans = true;  // Deliberately inserts orphans.

function runTest({coll, options = {}}) {
    // Test each query with different filters on shard key and non-shard key fields.
    const filters = [
        {},
        {shardKey: {$gte: "chunk1_s0_1"}},
        {notShardKey: {$gte: "1notShardKey_chunk1_s0_1"}},
    ];

    subSection("distinct on shard key");
    for (const filter of filters) {
        outputDistinctPlanAndResults(coll, "shardKey", filter, options);
    }

    subSection("$group on shard key");
    for (const filter of filters) {
        outputAggregationPlanAndResults(
            coll, [{$match: filter}, {$group: {_id: "$shardKey"}}], options);
    }

    coll.createIndex({notShardKey: 1});

    subSection("distinct on non-shard key field");
    for (const filter of filters) {
        outputDistinctPlanAndResults(coll, "notShardKey", filter, options);
    }

    subSection("$group on non-shard key field");
    for (const filter of filters) {
        outputAggregationPlanAndResults(
            coll, [{$match: filter}, {$group: {_id: "$notShardKey"}}], options);
    }
}

const readConcern = {
    level: "available"
};

{
    const {shardingTest, coll} = setupShardedCollectionWithOrphans();
    section("With readConcern 'available' set via command options");
    runTest({coll, options: {readConcern}});
    shardingTest.stop();
}

{
    const {shardingTest, coll} = setupShardedCollectionWithOrphans(readConcern);
    section("With 'available' as the default readConcern");
    note("Explain doesn't support default readConcern, so a sharding filter stage may appear in " +
         "the explain output even though the queries return orphans.");
    runTest({coll});
    shardingTest.stop();
}

{
    const {shardingTest, coll} = setupShardedCollectionWithOrphans();
    section("With readConcern 'majority' set via command options");
    runTest({coll, options: {readConcern: {level: "majority"}}});
    shardingTest.stop();
}
