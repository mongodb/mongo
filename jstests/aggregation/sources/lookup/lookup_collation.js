/**
 * Tests that $lookup respects the user-specified collation or the inherited local collation
 * when performing comparisons on a foreign collection with a different default collation. Exercises
 * the fix for SERVER-43350.
 *
 * Collation can be set at two different levels for $lookup stage
 *  1. on the local collection (collation on the foreign collection is always ignored)
 *  2. on the aggregation command via 'collation' property in options
 *
 * The 'collation' command option overrides local collection's collation.
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getAggPlanStages, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

// ToDo: SERVER-103530. Remove multiversion check when 9.0 becomes last-lts
const isMultiversion =
    Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const caseInsensitive = {
    locale: "en_US",
    strength: 1,
};

const caseSensitive = {
    locale: "simple",
};

// When no collation is specified for a collection, it uses the default, case-sensitive collation.
assert.commandWorked(testDB.createCollection("case_sensitive"));
const collAa = testDB.case_sensitive;
assert.commandWorked(testDB.createCollection("case_sensitive_indexed"));
const collAa_indexed = testDB.case_sensitive_indexed;

assert.commandWorked(testDB.createCollection("case_insensitive", {collation: caseInsensitive}));
const collAA = testDB.case_insensitive;

const records = [
    {_id: 0, key: "a"},
    {_id: 1, key: "A"},
];
assert.commandWorked(collAa.insert(records));
assert.commandWorked(collAA.insert(records));
assert.commandWorked(collAa_indexed.insert(records));
// Create two indexes to check the one with a matching collation will be chosen even if it has a
// longer key pattern.
assert.commandWorked(collAa_indexed.createIndex({key: 1}));
assert.commandWorked(collAa_indexed.createIndex({key: 1, x: 1}, {collation: caseInsensitive}));

const lookupWithPipeline = (foreignColl) => {
    return {
        $lookup: {
            from: foreignColl.getName(),
            as: "matched",
            let: {l_key: "$key"},
            pipeline: [{$match: {$expr: {$eq: ["$key", "$$l_key"]}}}],
        },
    };
};
const lookupNoPipeline = (foreignColl) => {
    return {
        $lookup: {from: foreignColl.getName(), localField: "key", foreignField: "key", as: "matched"},
    };
};

const resultCaseSensitive = [
    {_id: 0, key: "a", matched: [{_id: 0, key: "a"}]},
    {_id: 1, key: "A", matched: [{_id: 1, key: "A"}]},
];
const resultCaseInsensitive = [
    {
        _id: 0,
        key: "a",
        matched: [
            {_id: 0, key: "a"},
            {_id: 1, key: "A"},
        ],
    },
    {
        _id: 1,
        key: "A",
        matched: [
            {_id: 0, key: "a"},
            {_id: 1, key: "A"},
        ],
    },
];
let results = [];
let explain;

// Collation on the foreign collection should be ignored.
(function testLocalCollationPrecedence() {
    for (let lookupInto of [lookupWithPipeline, lookupNoPipeline]) {
        results = collAa.aggregate([lookupInto(collAA)]).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseSensitive,
            extraErrorMsg: " Default collation on local, running: " + tojson(lookupInto),
        });

        results = collAA.aggregate([lookupInto(collAa)], {allowDiskUse: false}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on local, running: " + tojson(lookupInto),
        });

        // When lowering to SBE a different join algorithm (HashJoin) is used if 'allowDiskUse' is
        // set to true. We only need to verify the collation of HJ once, because it works the same
        // independent of how the collation is chosen.
        results = collAA.aggregate([lookupInto(collAa)], {allowDiskUse: true}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on local, disk use allowed, running: " + tojson(lookupInto),
        });
    }
})();

// Collation at the command level should override collation of the local collection.
(function testCommandCollationPrecedence() {
    for (let lookupInto of [lookupWithPipeline, lookupNoPipeline]) {
        results = collAa.aggregate([lookupInto(collAa)], {collation: caseInsensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on command, running: " + tojson(lookupInto),
        });

        results = collAA.aggregate([lookupInto(collAa)], {collation: caseSensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseSensitive,
            extraErrorMsg: " Case-sensitive collation on command, running: " + tojson(lookupInto),
        });
    }
})();

// In presence of indexes, lookup might choose a different strategy for the join, that relies on the
// index (INLJ). It should respect the effective collation of $lookup. If there is an index with
// compatible collation, it should select an IndexedNestedLoopJoin. If there is no index
// and disk usage is allowed it should select HashLookup. If there is an index but it does not have
// compatible collation, it should select DynamicIndexedLoopJoin. In all other cases it should
// select NestedLoopJoin.
(function testCollationWithIndexes() {
    function assertJoinStrategy(explain, strategyName, indexName) {
        // Check join strategy when $lookup is pushed down.
        if (getAggPlanStages(explain, "$cursor").length === 0) {
            const winningPlan = getWinningPlanFromExplain(explain);
            if (winningPlan.stage === "EQ_LOOKUP") {
                assert.eq(strategyName, winningPlan.strategy, explain);
                if (indexName !== null) {
                    assert.eq(indexName, winningPlan.indexName, explain);
                }
            }
        }
    }

    function assertJoinStrategyMulti(explain, expectedStrategies) {
        // Check join strategy when $lookup is pushed down.
        if (getAggPlanStages(explain, "$cursor").length === 0) {
            const winningPlan = getWinningPlanFromExplain(explain);
            if (winningPlan.stage === "EQ_LOOKUP") {
                const strategies = Array.isArray(expectedStrategies) ? expectedStrategies : [expectedStrategies];

                let foundMatch = false;
                for (const expectedStrategy of strategies) {
                    if (winningPlan.strategy === expectedStrategy.name) {
                        foundMatch = true;
                        // If we found the strategy, verify the index
                        if (expectedStrategy.index !== null) {
                            // We care about the index - must be an exact match
                            assert.eq(expectedStrategy.index, winningPlan.indexName, explain);
                            break;
                        }
                    }
                }
                assert(foundMatch, explain);
            }
        }
    }

    for (let lookupInto of [lookupWithPipeline, lookupNoPipeline]) {
        // Local is case insensitive and foreign has an index with compatible collation (case
        // insensitive).
        results = collAA.aggregate([lookupInto(collAa_indexed)]).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on local, foreign is indexed, running: " + tojson(lookupInto),
        });
        let areCollectionsColocated = FixtureHelpers.areCollectionsColocated([collAA, collAa_indexed]);
        if (areCollectionsColocated) {
            explain = collAA.explain().aggregate([lookupInto(collAa_indexed)]);
            assertJoinStrategy(explain, "IndexedLoopJoin", "key_1_x_1");
        }

        // Command-level collation overrides the local collation and foreign has an index with
        // compatible collation (case insensitive).
        results = collAa.aggregate([lookupInto(collAa_indexed)], {collation: caseInsensitive}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseInsensitive,
            extraErrorMsg: " Case-insensitive collation on command, foreign is indexed, running:" + tojson(lookupInto),
        });
        areCollectionsColocated = FixtureHelpers.areCollectionsColocated([collAa, collAa_indexed]);
        if (areCollectionsColocated) {
            explain = collAa.explain().aggregate([lookupInto(collAa_indexed)], {collation: caseInsensitive});
            assertJoinStrategy(explain, "IndexedLoopJoin", "key_1_x_1");
        }

        // Command-level collation, {locale: "fr"}, overrides the local collation and foreign has an
        // index with incompatible collation.
        results = collAa.aggregate([lookupInto(collAa_indexed)], {collation: {locale: "fr"}}).toArray();
        assertArrayEq({
            actual: results,
            expected: resultCaseSensitive,
            extraErrorMsg: " locale fr collation on local, foreign is indexed,running: " + tojson(lookupInto),
        });
        areCollectionsColocated = FixtureHelpers.areCollectionsColocated([collAa, collAa_indexed]);
        if (areCollectionsColocated) {
            // If there is an index but it is not compatible with the requested collation and disk
            // usage is not allowed, dynamic indexed loop join will be chosen.
            explain = collAa
                .explain()
                .aggregate([lookupInto(collAa_indexed)], {allowDiskUse: false, collation: {locale: "fr"}});
            if (isMultiversion) {
                assertJoinStrategyMulti(explain, [
                    {name: "NestedLoopJoin", index: null},
                    {name: "DynamicIndexedLoopJoin", index: "key_1"},
                ]);
            } else {
                assertJoinStrategy(explain, "DynamicIndexedLoopJoin", "key_1");
            }
        }
        areCollectionsColocated = FixtureHelpers.areCollectionsColocated([collAa, collAa_indexed]);
        if (areCollectionsColocated) {
            // If there is an index but it is not compatible with the requested collation and disk
            // usage is allowed, hash join will be chosen.
            explain = collAa
                .explain()
                .aggregate([lookupInto(collAa_indexed)], {allowDiskUse: true, collation: {locale: "fr"}});
            assertJoinStrategy(explain, "HashJoin", null);
        }

        // There is no compatible index.
        areCollectionsColocated = FixtureHelpers.areCollectionsColocated([collAa, collAA]);
        if (areCollectionsColocated) {
            // If no index is compatible with the requested collation and disk use is
            // allowed, hash join will be chosen.
            explain = collAa.explain().aggregate([lookupInto(collAA)], {allowDiskUse: true});
            assertJoinStrategy(explain, "HashJoin", null);
        }
        areCollectionsColocated = FixtureHelpers.areCollectionsColocated([collAa, collAA]);
        if (areCollectionsColocated) {
            // If no index is compatible with the requested collation and disk use is not
            // allowed, nested loop join will be chosen.
            explain = collAa.explain().aggregate([lookupInto(collAA)], {allowDiskUse: false});
            assertJoinStrategy(explain, "NestedLoopJoin", null);
        }
    }

    // The compatible index is the _id index
    let pipeline = {
        $lookup: {
            from: collAA.getName(),
            as: "matched",
            let: {l_key: "$_id"},
            pipeline: [{$match: {$expr: {$eq: ["$_id", "$$l_key"]}}}],
        },
    };
    let areCollectionsColocated = FixtureHelpers.areCollectionsColocated([collAa, collAA]);
    if (areCollectionsColocated) {
        explain = collAa.explain().aggregate([pipeline]);
        assertJoinStrategy(explain, "HashJoin", null);
    }

    pipeline = {
        $lookup: {from: collAA.getName(), localField: "_id", foreignField: "_id", as: "matched"},
    };
    jsTest.log.info("Running pipeline: ", pipeline);
    areCollectionsColocated = FixtureHelpers.areCollectionsColocated([collAa, collAA]);
    if (areCollectionsColocated) {
        explain = collAa.explain().aggregate([pipeline]);
        assertJoinStrategy(explain, "HashJoin", null);
    }
})();
