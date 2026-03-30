/**
 * Verify that collection, index and query types unsupported by CBR fallback to multiplanning.
 */
import {
    getAllPlans,
    getPlanStages,
    getWinningPlanFromExplain,
    isExpress,
    isSubplannerCompositePlan,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, assertPlanNotCosted} from "jstests/libs/query/cbr_utils.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

// TODO SERVER-92589: Remove this exemption
if (checkSbeFullyEnabled(db)) {
    jsTestLog(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

const collName = jsTestName();
const coll = db[collName];
coll.drop();
// Ensure the collection is non-empty
assert.commandWorked(coll.insert({a: 1}));
assert.commandWorked(coll.insert({"2dsphere_loc": {type: "Point", coordinates: [-73, 40]}}));

/**
 * Return a boolean predicate function which takes a plan as a parameter and returns true if the
 * plan contains an IXSCAN stage over the given 'keyPattern'.
 */
function isPlanWithIndexScan(keyPattern) {
    return function (plan) {
        const stages = getPlanStages(plan, "IXSCAN");
        return stages.some((stage) => {
            // bsonWoCompare returns 0 when the objects are equal
            return bsonWoCompare(stage.keyPattern, keyPattern) === 0;
        });
    };
}

function testHashedIndex() {
    // Create hashed and non-hashed indexes on 'a'. Run a query on 'a' and check there are two
    // plans, one using either index.
    assert.commandWorked(coll.createIndexes([{a: 1}, {a: "hashed"}]));
    const explain = coll.find({a: 1}).explain();
    const plans = getAllPlans(explain);
    assert.eq(plans.length, 2);

    // Assert the plan not costed used the hashed index.
    plans.filter(isPlanWithIndexScan({a: "hashed"})).forEach(assertPlanNotCosted);

    assert.commandWorked(coll.dropIndexes());
}

function testPartialIndex() {
    // Create partial index on 'a' and regular index on 'b'. Query using a predicate containing 'a'
    // and 'b', resulting in three plans:
    // 1. Fetch(a) -> Ixscan(b)
    // 2. Fetch(b) -> Ixscan(a)
    // 3. Fetch -> IxIntersect [IxScan(a), Ixscan(b)]
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlannerEnableSortIndexIntersection: true}));
    assert.commandWorked(coll.createIndex({a: 1}, {partialFilterExpression: {a: {$gt: 10}}}));
    assert.commandWorked(coll.createIndex({b: 1}));
    const explain = coll.find({a: 20, b: 20}).explain();
    const plans = getAllPlans(explain);
    assert.eq(plans.length, 3);

    // Assert plans using partial 'a' index are not costed
    plans.filter(isPlanWithIndexScan({a: 1})).forEach(assertPlanNotCosted);

    assert.commandWorked(coll.dropIndexes());
}

function testSparseIndex() {
    assert.commandWorked(coll.createIndex({a: 1}, {sparse: true}));
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    const explain = coll.find({a: 20}).explain();
    const plans = getAllPlans(explain);
    // Verify sparse index was not costed
    plans.filter(isPlanWithIndexScan({a: 1})).forEach(assertPlanNotCosted);

    assert.commandWorked(coll.dropIndexes());
}

function testWildcardIndex() {
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({"$**": 1}));
    const explain = coll.find({a: 20}).explain();
    const plans = getAllPlans(explain);
    // Verify wildcard index was not costed
    plans.filter(isPlanWithIndexScan({$_path: 1, a: 1})).forEach(assertPlanNotCosted);

    assert.commandWorked(coll.dropIndexes());
}

function testV1Index() {
    assert.commandWorked(
        coll.runCommand("createIndexes", {
            indexes: [
                {key: {a: 1}, name: "a_1", v: 1},
                {key: {b: 1, a: 1}, name: "a1_b1"}, // v2 is the default
            ],
        }),
    );
    const explain = coll.find({a: 1, b: 1}).explain();
    const plans = getAllPlans(explain);
    // Verify v1 index was not costed
    plans.filter(isPlanWithIndexScan({a: 1})).forEach(assertPlanNotCosted);

    assert.commandWorked(coll.dropIndexes());
}

function testTextIndex() {
    // It seems that we never multi-plan with $text predicates which only ever enumerate a single
    // plan using the text index (of which there can only be one per collection). As a result, this
    // testcase just verifies that we don't cost the plan.
    assert.commandWorked(coll.createIndex({a: "text"}));
    {
        // Test fallback on TEXT_MATCH
        const explain = coll.find({$text: {$search: "abc"}}).explain();
        const plans = getAllPlans(explain);
        plans.forEach(assertPlanNotCosted);
    }
    {
        // Test fallback on TEXT_OR
        const explain = coll.find({$text: {$search: "a b c"}}, {score: {$meta: "textScore"}}).explain();
        const plans = getAllPlans(explain);
        plans.forEach(assertPlanNotCosted);
    }
    assert.commandWorked(coll.dropIndexes());
}

function test2dGeoIndex() {
    assert.commandWorked(coll.createIndex({a: "2d"}));
    assert.commandWorked(coll.createIndex({a: "2d", b: 1}));
    const explain = coll.find({a: {$near: [-73, 40], $maxDistance: 2}, b: 1}).explain();
    const plans = getAllPlans(explain);
    // Every plan must use the geo index, so assert that every plan is not costed
    plans.forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

function test2dSphereGeoIndex() {
    assert.commandWorked(coll.createIndex({"2dsphere_loc": "2dsphere"}));
    assert.commandWorked(coll.createIndex({"2dsphere_loc": "2dsphere", b: 1}));
    const explain = coll
        .find({
            "2dsphere_loc": {$near: {$geometry: {type: "Point", coordinates: [-73, 40]}, $maxDistance: 2}},
            b: 1,
        })
        .explain();
    const plans = getAllPlans(explain);
    // Every plan must use the geo index, so assert that every plan is not costed
    plans.forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

function testIndexCollation() {
    const collation = {locale: "en", strength: 2};
    assert.commandWorked(coll.createIndex({a: 1}, {collation: collation}));
    const explain = coll.find({a: "abc"}).collation(collation).explain();
    const plans = getAllPlans(explain);
    plans.forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

function testClusteredIndex() {
    assert.commandWorked(
        db.createCollection("clusteredColl", {
            clusteredIndex: {"key": {_id: 1}, unique: true, name: "clustered_index"},
        }),
    );
    const clusteredColl = db.clusteredColl;
    clusteredColl.insert({_id: 1});
    {
        // This query ends up running through the express path and should not be costed
        const explain = clusteredColl.find({_id: 1}, {a: 1}).explain();
        getAllPlans(explain).forEach(assertPlanNotCosted);
    }
    {
        // This query is not eligible for express and runs through the query planner, verify that we
        // do not cost it. This is a regression test for SERVER-99690.
        const explain = clusteredColl.find({_id: 1}, {"a.b": 1}).explain();
        const plans = getAllPlans(explain);
        plans
            .filter((plan) => {
                assert(!isExpress(db, plan), plan);
                return getPlanStages(plan, "CLUSTERED_IXSCAN").length > 0;
            })
            .forEach(assertPlanNotCosted);
    }
    clusteredColl.drop();
}

function testMinMaxIndexScan() {
    // Min/max require a hint, so we'll only end up with one plan to verify is not costed.
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    const explain = coll.find().hint({a: 1, b: 1}).min({a: 0, b: 0}).max({a: 10, b: 0}).explain();
    const plans = getAllPlans(explain);
    plans.forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

function testReturnKey() {
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));
    const explain = coll.find().returnKey(true).explain();
    getAllPlans(explain).forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

function testSortKeyGenerator() {
    assert.commandWorked(coll.createIndex({a: 1}));
    const explain = coll
        .find({}, {a: {$meta: "sortKey"}})
        .sort({a: 1})
        .explain();
    getAllPlans(explain).forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

// Maximum $in-list size that CBR will estimate. Must match plan_ranking::kMaxInListSize.
const kMaxInListSize = 2048;

function testLargeInList() {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < kMaxInListSize + 1000; i++) {
        bulk.insert({a: i, b: i % 100});
    }
    assert.commandWorked(bulk.execute());

    // Two indexes on different fields so the planner enumerates at least two plans
    // (one per index) when both fields appear in the query predicate.
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

    // Use samplingCE so the test does not depend on histograms.
    const prevCEMode = assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryCBRCEMode: "samplingCE"}));

    // Two non-overlapping small $in-lists and one large $in-list reused across all sub-tests.
    const smallIn1 = Array.from({length: 100}, (_, i) => i);
    const smallIn2 = Array.from({length: 100}, (_, i) => i + 100);
    const largeIn = Array.from({length: kMaxInListSize + 1}, (_, i) => i);

    // Runs a find query and asserts that all enumerated plans are costed (or not costed).
    function testQuery(query, cbrExpected) {
        const explain = coll.find(query).explain();
        const plans = getAllPlans(explain);
        assert.gte(plans.length, 2, "Expected at least two plans");
        plans.forEach(cbrExpected ? assertPlanCosted : assertPlanNotCosted);
    }

    // Small $in-list: CBR should be able to estimate it.
    testQuery({a: {$in: smallIn1}, b: {$lt: 50}}, true);

    // Large $in-list (> kMaxInListSize elements): CBR should fall back to multiplanning.
    testQuery({a: {$in: largeIn}, b: {$lt: 50}}, false);

    // $or with small $in in both branches: CBR should be used.
    // The $or is combined with a top-level predicate on 'b' so the query goes through the regular
    // planner (not subplanning). With indexes {a: 1} and {b: 1}, the planner enumerates at
    // least two candidate plans. containsLargeInList walks the full expression tree, so it detects
    // $in-lists inside $or branches.
    testQuery({b: {$lt: 50}, $or: [{a: {$in: smallIn1}}, {a: {$in: smallIn2}}]}, true);

    // $or where the second branch has a large $in-list: should fall back to multiplanning.
    testQuery({b: {$lt: 50}, $or: [{a: {$in: smallIn1}}, {a: {$in: largeIn}}]}, false);

    // Rooted $or (subplanner path): each branch has predicates on 'a' and 'b' so that with
    // indexes {a: 1} and {b: 1} each branch independently has at least two candidate plans.
    // The subplanner combines per-branch winners into a single composite plan. We verify the
    // subplanner is used and the plan structure is correct. Note: the subplanner composite
    // explain does not expose costEstimate on per-branch plans, so we can only exercise the code
    // path, veryfy it via other means, and verify structural plan properties here.
    function testRootedOrQuery(query) {
        const explain = coll.find(query).explain();
        assert(isSubplannerCompositePlan(explain), "Expected subplanner composite plan");
        const winningPlan = getWinningPlanFromExplain(explain);
        assert(planHasStage(db, winningPlan, "OR"), "Expected OR stage in subplanner composite plan");
        const ixscans = getPlanStages(winningPlan, "IXSCAN");
        assert.eq(ixscans.length, 2, "Expected one IXSCAN per $or branch");
    }

    // Rooted $or with small $in in both branches: exercises the CBR-per-branch code path.
    testRootedOrQuery({
        $or: [
            {a: {$in: smallIn1}, b: {$lt: 50}},
            {a: {$in: smallIn2}, b: {$lt: 50}},
        ],
    });

    // Rooted $or where the second branch has a large $in: exercises the multiplanning-per-branch
    // fallback code path in the subplanner.
    testRootedOrQuery({
        $or: [
            {a: {$in: smallIn1}, b: {$lt: 50}},
            {a: {$in: largeIn}, b: {$lt: 50}},
        ],
    });

    // Restore CE mode for the remaining tests.
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryCBRCEMode: prevCEMode.was}));
    assert.commandWorked(coll.dropIndexes());
}

function testDistictScan() {
    assert.commandWorked(coll.createIndex({a: 1, b: 1}));
    const explain = coll.explain().aggregate([{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", f: {$first: "$b"}}}]);
    const plans = getAllPlans(explain);
    assert.gt(plans.length, 0);
    plans.forEach((plan) => {
        const distinctScanStages = getPlanStages(plan, "DISTINCT_SCAN");
        assert.neq(0, distinctScanStages.length, plan);
    });
    plans.forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

try {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: true, internalQueryCBRCEMode: "heuristicCE"}),
    );

    testHashedIndex();
    testPartialIndex();
    testSparseIndex();
    testWildcardIndex();
    testV1Index();
    testTextIndex();
    test2dGeoIndex();
    test2dSphereGeoIndex();
    testIndexCollation();
    testClusteredIndex();
    testMinMaxIndexScan();
    testReturnKey();
    testSortKeyGenerator();
    testDistictScan();
    testLargeInList();
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, featureFlagCostBasedRanker: false}));
    assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlannerEnableSortIndexIntersection: false}));
}
