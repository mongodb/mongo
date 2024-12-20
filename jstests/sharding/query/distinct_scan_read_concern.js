/**
 * Tests that DISTINCT_SCAN queries against a sharded collection don't apply shard filtering when
 * the read concern level is "available".
 *
 * @tags: [
 *   featureFlagShardFilteringDistinctScan
 * ]
 */
import {getPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {setupShardedCollectionWithOrphans} from "jstests/libs/query_golden_sharding_utils.js";

TestData.skipCheckOrphans = true;  // Deliberately inserts orphans.

function validateResults({results, nonOrphanDocuments, mayReturnOrphans}) {
    const asStrings = results.map(x => tojson(x));
    assert.eq(
        new Set(asStrings).size,
        results.length,
        "Results are not distinct: " + tojson(results),
    );

    const isOrphan = value => value.includes("orphan");
    const nonOrphansFound = asStrings.reduce(
        (acc, value) => acc + (isOrphan(value) ? 0 : 1),
        0,
    );
    assert.eq(nonOrphansFound, nonOrphanDocuments, tojson(results));

    if (!mayReturnOrphans) {
        assert.eq(results.length, nonOrphanDocuments, tojson(results));
    }
}

function validateExplain({explain, shouldHaveShardFilteringStage, shouldHaveDistinctScan}) {
    const winningPlan = getWinningPlanFromExplain(explain);
    const distinctScans = getPlanStages(winningPlan, "DISTINCT_SCAN");

    const hasDistinctScan = distinctScans.length > 0;
    assert(!shouldHaveDistinctScan || hasDistinctScan, tojson(explain));

    const isShardFiltering = hasDistinctScan
        ? distinctScans[0].isShardFiltering
        : getPlanStages(winningPlan, "SHARDING_FILTER").length > 0;
    assert.eq(isShardFiltering, shouldHaveShardFilteringStage, tojson(explain));
}

function runDistinctTest({
    coll,
    key,
    filter,
    options,
    nonOrphanDocuments,
    mayReturnOrphans,
    shouldHaveShardFilteringStage,
    shouldHaveDistinctScan,
}) {
    // The 'coll.distinct()' shell helper doesn't support some options like 'readConcern', even
    // though the command does support them.
    const cmdArgs = {distinct: coll.getName(), key, query: filter, ...options};
    const results = assert.commandWorked(coll.getDB().runCommand(cmdArgs)).values;
    const explain = assert.commandWorked(
        coll.getDB().runCommand({explain: cmdArgs, verbosity: "queryPlanner"}));

    validateResults({results, mayReturnOrphans, nonOrphanDocuments});
    validateExplain(
        {explain, shouldHaveShardFilteringStage, shouldHaveDistinctScan: shouldHaveDistinctScan});
}

function runAggregationTest({
    coll,
    key,
    filter,
    sort,
    options,
    nonOrphanDocuments,
    mayReturnOrphans,
    shouldHaveShardFilteringStage,
    shouldHaveDistinctScan,
}) {
    const runTest = pipe => {
        const results = coll.aggregate(pipe, options).toArray();
        const explain = coll.explain().aggregate(pipe, options);

        validateResults({results, mayReturnOrphans, nonOrphanDocuments});
        validateExplain({explain, shouldHaveShardFilteringStage, shouldHaveDistinctScan});
    };

    runTest([{$match: filter}, {$group: {_id: "$" + key}}]);
    if (sort) {
        runTest([{$sort: sort}, {$match: filter}, {$group: {_id: "$" + key}}]);
        runTest([
            {$match: filter},
            {$group: {_id: "$" + key, accum: {$top: {sortBy: sort, output: "$" + key}}}}
        ]);
    }
}

const filterOnShardKey = {
    shardKey: {$gte: "chunk1_s0_1"}
};
const filterOnNonShardKey = {
    notShardKey: {$gte: "1notShardKey_chunk1_s0_1"}
};

const testCases = [
    {
        key: "shardKey",
        filter: {},
        sort: {shardKey: 1},
        nonOrphanDocuments: 18,
        shouldHaveDistinctScan: true,
    },
    {
        key: "shardKey",
        filter: filterOnShardKey,
        sort: {shardKey: 1},
        nonOrphanDocuments: 17,
        shouldHaveDistinctScan: true,
    },
    {
        key: "shardKey",
        filter: filterOnNonShardKey,
        sort: {notShardKey: 1},
        nonOrphanDocuments: 18,
        shouldHaveDistinctScan: false,
    },
    {
        key: "notShardKey",
        filter: {},
        sort: {notShardKey: 1},
        nonOrphanDocuments: 54,
        shouldHaveDistinctScan: true,
        // TODO SERVER-98389: This should always be distinct scan eligible.
        shouldHaveDistinctScanDistinctOnly: true,
    },
    {
        key: "notShardKey",
        filter: filterOnShardKey,
        sort: {shardKey: 1},
        nonOrphanDocuments: 51,
        shouldHaveDistinctScan: true,
        // TODO SERVER-98389: This should always be distinct scan eligible.
        shouldHaveDistinctScanDistinctOnly: true,
    },
    {
        key: "notShardKey",
        filter: filterOnNonShardKey,
        sort: {notShardKey: 1},
        nonOrphanDocuments: 53,
        shouldHaveDistinctScan: true,
        // TODO SERVER-98389: This should always be distinct scan eligible.
        shouldHaveDistinctScanDistinctOnly: true,
    },
];

function runTestsWithOptions(
    {coll, options = {}, mayReturnOrphans, shouldHaveShardFilteringStage}) {
    const commonTestProps = {coll, options, mayReturnOrphans, shouldHaveShardFilteringStage};
    for (const testCase of testCases) {
        print("Running distinct test: " + tojson(testCase));
        runDistinctTest({...commonTestProps, ...testCase});

        print("Running aggregation test: " + tojson(testCase));
        runAggregationTest({
            ...commonTestProps,
            ...testCase,
            shouldHaveDistinctScan:
                testCase.shouldHaveDistinctScan && !testCase.shouldHaveDistinctScanDistinctOnly
        });
    }
}

const readConcernAvailable = {
    level: "available"
};

const {shardingTest, coll} = setupShardedCollectionWithOrphans();
// The setup function already created indexes 'shardKey_1' and 'shardKey_1_notShardKey_1'.
coll.createIndex({notShardKey: 1});

print("Running test with readConcern 'available' set via command options");
runTestsWithOptions({
    coll,
    options: {readConcern: readConcernAvailable},
    mayReturnOrphans: true,
    shouldHaveShardFilteringStage: false,
});

print("Running test with readConcern 'majority' set via command options");
runTestsWithOptions({
    coll,
    options: {readConcern: {level: "majority"}},
    mayReturnOrphans: false,
    shouldHaveShardFilteringStage: true,
});

shardingTest.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: readConcernAvailable});
print("Running test with 'available' as the default readConcern");
runTestsWithOptions({
    coll,
    mayReturnOrphans: true,
    // TODO SERVER-98047: Explain doesn't currently support custom default readConcerns, so we
    // will incorrectly see a SHARDING_FILTER in the explain output.
    shouldHaveShardFilteringStage: true,
});

shardingTest.stop();
