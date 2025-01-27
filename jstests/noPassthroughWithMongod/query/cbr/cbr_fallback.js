/**
 * Verify that collection, index and query types unsupported by CBR fallback to multiplanning.
 */
import {
    getAllPlans,
    getPlanStages,
} from "jstests/libs/query/analyze_plan.js";
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
assert.commandWorked(coll.insert({'2dsphere_loc': {type: 'Point', coordinates: [-73, 40]}}));

/**
 * Return a boolean predicate function which takes a plan as a parameter and returns true if the
 * plan contains an IXSCAN stage over the given 'keyPattern'.
 */
function isPlanWithIndexScan(keyPattern) {
    return function(plan) {
        const stages = getPlanStages(plan, "IXSCAN");
        return stages.some(stage => {
            // bsonWoCompare returns 0 when the objects are equal
            return bsonWoCompare(stage.keyPattern, keyPattern) === 0;
        });
    };
}

/**
 * Assert the given plan was not costed. This is used as an indicator to whether CBR used its
 * fallback to multiplanning for this plan.
 */
function assertPlanNotCosted(plan) {
    assert(!plan.hasOwnProperty('costEstimate'), plan);
}

function testHashedIndex() {
    // Create hashed and non-hashed indexes on 'a'. Run a query on 'a' and check there are two
    // plans, one using either index.
    assert.commandWorked(coll.createIndexes([{a: 1}, {a: 'hashed'}]));
    const explain = coll.find({a: 1}).explain();
    const plans = getAllPlans(explain);
    assert.eq(plans.length, 2);

    // Assert the plan not costed used the hashed index.
    plans.filter(isPlanWithIndexScan({a: 'hashed'})).forEach(assertPlanNotCosted);

    assert.commandWorked(coll.dropIndexes());
}

function testPartialIndex() {
    // Create partial index on 'a' and regular index on 'b'. Query using a predicate containing 'a'
    // and 'b', resulting in three plans:
    // 1. Fetch(a) -> Ixscan(b)
    // 2. Fetch(b) -> Ixscan(a)
    // 3. Fetch -> IxIntersect [IxScan(a), Ixscan(b)]
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
    assert.commandWorked(coll.createIndex({'$**': 1}));
    const explain = coll.find({a: 20}).explain();
    const plans = getAllPlans(explain);
    // Verify wildcard index was not costed
    plans.filter(isPlanWithIndexScan({$_path: 1, a: 1})).forEach(assertPlanNotCosted);

    assert.commandWorked(coll.dropIndexes());
}

function testV1Index() {
    assert.commandWorked(coll.runCommand('createIndexes', {
        indexes: [
            {key: {a: 1}, name: 'a_1', v: 1},
            {key: {b: 1, a: 1}, name: 'a1_b1'},  // v2 is the default
        ]
    }));
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
    assert.commandWorked(coll.createIndex({a: 'text'}));
    {
        // Test fallback on TEXT_MATCH
        const explain = coll.find({$text: {$search: 'abc'}}).explain();
        const plans = getAllPlans(explain);
        plans.forEach(assertPlanNotCosted);
    }
    {
        // Test fallback on TEXT_OR
        const explain =
            coll.find({$text: {$search: 'a b c'}}, {score: {$meta: 'textScore'}}).explain();
        const plans = getAllPlans(explain);
        plans.forEach(assertPlanNotCosted);
    }
    assert.commandWorked(coll.dropIndexes());
}

function test2dGeoIndex() {
    assert.commandWorked(coll.createIndex({a: '2d'}));
    assert.commandWorked(coll.createIndex({a: '2d', b: 1}));
    const explain = coll.find({a: {$near: [-73, 40], $maxDistance: 2}, b: 1}).explain();
    const plans = getAllPlans(explain);
    // Every plan must use the geo index, so assert that every plan is not costed
    plans.forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

function test2dSphereGeoIndex() {
    assert.commandWorked(coll.createIndex({'2dsphere_loc': '2dsphere'}));
    assert.commandWorked(coll.createIndex({'2dsphere_loc': '2dsphere', b: 1}));
    const explain =
        coll.find({
                '2dsphere_loc':
                    {$near: {$geometry: {type: 'Point', coordinates: [-73, 40]}, $maxDistance: 2}},
                b: 1
            })
            .explain();
    const plans = getAllPlans(explain);
    // Every plan must use the geo index, so assert that every plan is not costed
    plans.forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

function testIndexCollation() {
    const collation = {locale: 'en', strength: 2};
    assert.commandWorked(coll.createIndex({a: 1}, {collation: collation}));
    const explain = coll.find({a: "abc"}).collation(collation).explain();
    const plans = getAllPlans(explain);
    plans.forEach(assertPlanNotCosted);
    assert.commandWorked(coll.dropIndexes());
}

function testClusteredIndex() {
    assert.commandWorked(db.createCollection(
        "clusteredColl",
        {clusteredIndex: {"key": {_id: 1}, unique: true, name: "clustered_index"}}));
    const clusteredColl = db.clusteredColl;
    assert.commandWorked(clusteredColl.createIndex({a: 1}));
    clusteredColl.insert({_id: 1});
    const explain = clusteredColl.find({_id: 1, a: 1}).explain();
    const plans = getAllPlans(explain);
    plans
        .filter(plan => {
            return getPlanStages(plan, "CLUSTERED_IXSCAN").length > 0;
        })
        .forEach(assertPlanNotCosted);
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

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "heuristicCE"}));

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
} finally {
    // Ensure that query knob doesn't leak into other testcases in the suite.
    assert.commandWorked(db.adminCommand({setParameter: 1, planRankerMode: "multiPlanning"}));
}
