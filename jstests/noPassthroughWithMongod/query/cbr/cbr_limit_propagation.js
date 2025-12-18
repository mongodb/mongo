/**
 * Test that a $limit stage updates the cardinality estimates of child stages.
 */

import helpers, {
    ce,
    keyPattern,
    numKeys,
    filter,
} from "jstests/noPassthroughWithMongod/query/cbr/cbr_expect_helpers.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

if (checkSbeFullyEnabled(db)) {
    jsTest.log.info(`Skipping ${jsTestName()} as SBE executor is not supported yet`);
    quit();
}

assert.commandWorked(
    db.adminCommand({setParameter: 1, planRankerMode: "samplingCE", internalQuerySamplingBySequentialScan: true}),
);

const coll = db[jsTestName()];
coll.drop();

coll.insertMany(
    [...Array(1000).keys()].map((i) => ({
        _id: i,
        foo: i,
        foo_str: "foobar" + i,
        bar: i % 2,
        baz: i % 3,
        unindexed_baz: i % 3,
    })),
);
coll.createIndex({foo: 1});
coll.createIndex({foo_str: 1});
coll.createIndex({bar: 1});
coll.createIndex({baz: 1});

function getOnlyPlan(pipeline) {
    const explain = coll.explain().aggregate(pipeline);
    const plans = helpers.getPlans(explain);
    assert(plans.length == 1, {msg: "pipeline produces unexpected number of plans", pipeline, plans});
    return plans[0];
}

function getPlanWithStage(pipeline, stage) {
    const explain = coll.explain().aggregate(pipeline);
    const plans = helpers.getPlansWithStage(explain, stage);
    assert(plans.length > 0, {msg: "pipeline does not produce a plan with requested stage", pipeline, stage});
    return plans[0];
}

// COLLSCAN
getOnlyPlan([{"$match": {}}]).expect("COLLSCAN", ce.eq(1000));
getOnlyPlan([{"$match": {}}, {"$limit": 100}])
    .expect("LIMIT", ce.eq(100))
    .expect("COLLSCAN", ce.eq(100));

// IXSCAN
getOnlyPlan([{"$match": {"foo": {"$gt": 0}}}]).expect("IXSCAN", ce.near(999), numKeys.near(999));
getOnlyPlan([{"$match": {"foo": {"$gt": 0}}}, {"$limit": 100}])
    .expect("LIMIT", ce.eq(100))
    .expect("IXSCAN", ce.eq(100), numKeys.eq(100));

// Limit then skip
// NOTE: Skip will be pushed _before_ the limit.
const limitThenSkip = getOnlyPlan([{"$limit": 50}, {"$skip": 10}])
    .expect("LIMIT", ce.eq(40))
    .expect("SKIP", ce.eq(40))
    .expect("COLLSCAN", ce.eq(50));

assert(limitThenSkip.name == "LIMIT");
assert(limitThenSkip.children[0].name == "SKIP");

// Skip then limit
const skipThenLimit = getOnlyPlan([{"$skip": 10}, {"$limit": 50}])
    .expect("LIMIT", ce.eq(50))
    .expect("SKIP", ce.eq(50))
    .expect("COLLSCAN", ce.eq(60));

assert(skipThenLimit.name == "LIMIT");
assert(skipThenLimit.children[0].name == "SKIP");

// FETCH with a filter
const baz_eq_0 = {"unindexed_baz": {"$eq": 0}};
getOnlyPlan([{"$match": {"bar": 0}}, {"$match": baz_eq_0}])
    .expect("FETCH", filter(baz_eq_0), ce.near(166)) // bar == 0 && baz == 0 matches 1/6 of docs, ~166
    .expect("IXSCAN", keyPattern({bar: 1}), ce.near(500), numKeys.near(500)); // IXSCAN over bar == 0 matches 1/2 of docs
getOnlyPlan([{"$match": {"bar": 0}}, {"$match": baz_eq_0}, {"$limit": 10}])
    .expect("FETCH", filter(baz_eq_0), ce.near(10))
    // The FETCH with filter baz == 0 has selectivity ~= 166/500 ~= 0.33
    // To propagate the limit to the IXSCAN _before_ that filter, the limit must be scaled up.
    // We expect the IXSCAN to, on average, produce 30 values by the time the FETCH matches 10 (the
    // limit).
    .expect("IXSCAN", keyPattern({bar: 1}), ce.near(30), ce.near(10 * (500 / 166)), numKeys.near(10 * (500 / 166)));

// IXSCAN with a residual filter
// Regex will match ~1/10 values (incrementing counter, checking last digit == 0)
const foo_regex = {"foo_str": {"$regex": "^foobar.*0$"}};
getOnlyPlan([{"$match": foo_regex}])
    // Fetch does not need to filter anything
    .expect("FETCH", filter(undefined), ce.near(100))
    // IXSCAN can scan the key prefix foobar, but will have a residual filter to apply the full regex
    .expect("IXSCAN", keyPattern({foo_str: 1}), filter(foo_regex), ce.near(100), numKeys.near(1000));

getOnlyPlan([{"$match": foo_regex}, {"$limit": 10}])
    .expect("FETCH", filter(undefined), ce.eq(10))
    // IXSCAN has residual filter, so ce != numKeysEstimate, but limit can still be propagated back.
    .expect("IXSCAN", keyPattern({foo_str: 1}), filter(foo_regex), ce.eq(10), numKeys.near(100));

// Enable sort-based index intersection and force index intersections plans.
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlannerEnableSortIndexIntersection: true}));
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: true}));

// Validate that limits are scaled up proportionately when applied to the children of an AND
getPlanWithStage([{"$match": {bar: 1, baz: 1}}], "AND_SORTED")
    .expect("FETCH", ce.near(80))
    .expect("AND_SORTED", ce.near(235))
    .expect("IXSCAN", keyPattern({bar: 1}), ce.near(500))
    .expect("IXSCAN", keyPattern({baz: 1}), ce.near(333));
getPlanWithStage([{"$match": {bar: 1, baz: 1}}, {"$limit": 10}], "AND_SORTED")
    .expect("FETCH", ce.eq(10))
    .expect("AND_SORTED", ce.near(235 * (10 / 80)))
    .expect("IXSCAN", keyPattern({bar: 1}), ce.near(500 * (10 / 80)))
    .expect("IXSCAN", keyPattern({baz: 1}), ce.near(333 * (10 / 80)));

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryPlannerEnableSortIndexIntersection: false}));
assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryForceIntersectionPlans: false}));

// TODO: Test OR; currently subplanner will be used, and cardinality is not estimated.
