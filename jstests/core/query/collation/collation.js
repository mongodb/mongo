// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: applyOps,
//   # cloneCollectionAsCapped, mapReduce, reIndex.
//   not_allowed_with_signed_security_token,
//   assumes_no_implicit_collection_creation_after_drop,
//   # Asserts that some queries use a collection scan.
//   assumes_no_implicit_index_creation,
//   does_not_support_stepdowns,
//   requires_capped,
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
//   requires_scripting,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
//   requires_getmore,
// ]

// Integration tests for the collation feature.
import {ClusteredCollectionUtil} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {IndexCatalogHelpers} from "jstests/libs/index_catalog_helpers.js";
import {
    getPlanStage,
    getPlanStages,
    getSingleNodeExplain,
    getWinningPlanFromExplain,
    isCollscan,
    isIdhackOrExpress,
    isIxscan,
    planHasStage,
} from "jstests/libs/query/analyze_plan.js";

let testDb = db.getSiblingDB("collation_js");
let coll = testDb.collation;
coll.drop();

let explainRes;
let writeRes;
let planStage;

let hello = testDb.runCommand("hello");
assert.commandWorked(hello);
let isMongos = hello.msg === "isdbgrid";
let isStandalone = !isMongos && !hello.hasOwnProperty("setName");
let isClustered = ClusteredCollectionUtil.areAllCollectionsClustered(testDb);

let assertIndexHasCollation = function (keyPattern, collation) {
    let indexSpecs = coll.getIndexes();
    let found = IndexCatalogHelpers.findByKeyPattern(indexSpecs, keyPattern, collation);
    assert.neq(
        null,
        found,
        "Index with key pattern " +
            tojson(keyPattern) +
            " and collation " +
            tojson(collation) +
            " not found: " +
            tojson(indexSpecs),
    );
};

let getQueryCollation = function (explainRes) {
    if (explainRes.queryPlanner.hasOwnProperty("collation")) {
        return explainRes.queryPlanner.collation;
    }

    const winningPlan = getWinningPlanFromExplain(explainRes.queryPlanner);
    if (
        winningPlan.hasOwnProperty("shards") &&
        winningPlan.shards.length > 0 &&
        winningPlan.shards[0].hasOwnProperty("collation")
    ) {
        return winningPlan.shards[0].collation;
    }

    return null;
};

function distinctScanPlanHasFetch(winningPlan) {
    // On upgrade/downgrade suites, we can't directly check the feature flag, because it might
    // change between when we check its value, and when the query is executed. Instead, we check for
    // the presence/absence of a flag that should only be present when the flag is on.
    const ds = getPlanStages(winningPlan, "DISTINCT_SCAN")[0];
    const hasFetch = planHasStage(testDb, winningPlan, "FETCH");
    if (ds.hasOwnProperty("isFetching")) {
        // If this flag is present + we have a separate distinct scan, we should not have a separate
        // fetch stage.
        assert(!hasFetch);
        return ds.isFetching;
    }

    return hasFetch;
}

//
// Test using testDb.createCollection() to make a collection with a default collation.
//

// Attempting to create a collection with an invalid collation should fail.
assert.commandFailed(testDb.createCollection("collation", {collation: "not an object"}));
assert.commandFailed(testDb.createCollection("collation", {collation: {}}));
assert.commandFailed(testDb.createCollection("collation", {collation: {blah: 1}}));
assert.commandFailed(testDb.createCollection("collation", {collation: {locale: "en", blah: 1}}));
assert.commandFailed(testDb.createCollection("collation", {collation: {locale: "xx"}}));
assert.commandFailed(testDb.createCollection("collation", {collation: {locale: "en", strength: 99}}));
assert.commandFailed(testDb.createCollection("collation", {collation: {locale: "en", strength: 9.9}}));

// Attempting to create a collection whose collation version does not match the collator version
// produced by ICU should result in failure with a special error code.
assert.commandFailedWithCode(
    testDb.createCollection("collation", {collation: {locale: "en", version: "unknownVersion"}}),
    ErrorCodes.IncompatibleCollationVersion,
);

// Ensure we can create a collection with the "simple" collation as the collection default.
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "simple"}}));
var collectionInfos = testDb.getCollectionInfos({name: coll.getName()});
assert.eq(collectionInfos.length, 1);
assert(!collectionInfos[0].options.hasOwnProperty("collation"));

// Ensure that we populate all collation-related fields when we create a collection with a valid
// collation.
coll = testDb.collation_frCA;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
var collectionInfos = testDb.getCollectionInfos({name: coll.getName()});
assert.eq(collectionInfos.length, 1);
assert.eq(collectionInfos[0].options.collation, {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

// Ensure that an index with no collation inherits the collection-default collation.
assert.commandWorked(coll.createIndex({a: 1}));
assertIndexHasCollation(
    {a: 1},
    {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    },
);

// Ensure that an index which specifies an overriding collation does not use the collection
// default.
assert.commandWorked(coll.createIndex({b: 1}, {collation: {locale: "en_US"}}));
assertIndexHasCollation(
    {b: 1},
    {
        locale: "en_US",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: false,
        version: "57.1",
    },
);

// Ensure that an index which specifies the "simple" collation as an overriding collation still
// does not use the collection default.
assert.commandWorked(coll.createIndex({d: 1}, {collation: {locale: "simple"}}));
assertIndexHasCollation({d: 1}, {locale: "simple"});

// Ensure that a v=1 index doesn't inherit the collection-default collation.
assert.commandWorked(coll.createIndex({c: 1}, {v: 1}));
assertIndexHasCollation({c: 1}, {locale: "simple"});

// Test that all indexes retain their current collation when the collection is re-indexed. (Only
// standalone mode supports the reIndex command.)
if (isStandalone) {
    assert.commandWorked(coll.reIndex());
    assertIndexHasCollation(
        {a: 1},
        {
            locale: "fr_CA",
            caseLevel: false,
            caseFirst: "off",
            strength: 3,
            numericOrdering: false,
            alternate: "non-ignorable",
            maxVariable: "punct",
            normalization: false,
            backwards: true,
            version: "57.1",
        },
    );
    assertIndexHasCollation(
        {b: 1},
        {
            locale: "en_US",
            caseLevel: false,
            caseFirst: "off",
            strength: 3,
            numericOrdering: false,
            alternate: "non-ignorable",
            maxVariable: "punct",
            normalization: false,
            backwards: false,
            version: "57.1",
        },
    );
    assertIndexHasCollation({d: 1}, {locale: "simple"});
    assertIndexHasCollation({c: 1}, {locale: "simple"});
}

coll = testDb.collation_index1;
coll.drop();
//
// Creating an index with a collation.
//

// Attempting to build an index with an invalid collation should fail.
assert.commandFailed(coll.createIndex({a: 1}, {collation: "not an object"}));
assert.commandFailed(coll.createIndex({a: 1}, {collation: {}}));
assert.commandFailed(coll.createIndex({a: 1}, {collation: {blah: 1}}));
assert.commandFailed(coll.createIndex({a: 1}, {collation: {locale: "en", blah: 1}}));
assert.commandFailed(coll.createIndex({a: 1}, {collation: {locale: "xx"}}));
assert.commandFailed(coll.createIndex({a: 1}, {collation: {locale: "en", strength: 99}}));

// Attempting to create an index whose collation version does not match the collator version
// produced by ICU should result in failure with a special error code.
assert.commandFailedWithCode(
    coll.createIndex({a: 1}, {collation: {locale: "en", version: "unknownVersion"}}),
    ErrorCodes.IncompatibleCollationVersion,
);

assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "en_US"}}));
assertIndexHasCollation(
    {a: 1},
    {
        locale: "en_US",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: false,
        version: "57.1",
    },
);

assert.commandWorked(coll.createIndex({b: 1}, {collation: {locale: "en_US"}}));
assertIndexHasCollation(
    {b: 1},
    {
        locale: "en_US",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: false,
        version: "57.1",
    },
);

assert.commandWorked(coll.createIndexes([{c: 1}, {d: 1}], {collation: {locale: "fr_CA"}}));
assertIndexHasCollation(
    {c: 1},
    {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    },
);
assertIndexHasCollation(
    {d: 1},
    {
        locale: "fr_CA",
        caseLevel: false,
        caseFirst: "off",
        strength: 3,
        numericOrdering: false,
        alternate: "non-ignorable",
        maxVariable: "punct",
        normalization: false,
        backwards: true,
        version: "57.1",
    },
);

assert.commandWorked(coll.createIndexes([{e: 1}], {collation: {locale: "simple"}}));
assertIndexHasCollation({e: 1}, {locale: "simple"});

// Test that an index with a non-simple collation contains collator-generated comparison keys
// rather than the verbatim indexed strings.
if (!TestData.isHintsToQuerySettingsSuite) {
    // This guard excludes this test case from being run on the cursor_hints_to_query_settings
    // suite. The suite replaces cursor hints with query settings. Query settings do not force
    // indexes, and therefore empty filter will result in collection scans.
    coll = testDb.collation_index2;
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr_CA"}}));
    assert.commandWorked(coll.createIndex({b: 1}));
    assert.commandWorked(coll.insert({a: "foo", b: "foo"}));
    assert.eq(1, coll.find().collation({locale: "fr_CA"}).hint({a: 1}).returnKey().itcount());
    assert.neq("foo", coll.find().collation({locale: "fr_CA"}).hint({a: 1}).returnKey().next().a);
    assert.eq(1, coll.find().collation({locale: "fr_CA"}).hint({b: 1}).returnKey().itcount());
    assert.eq("foo", coll.find().collation({locale: "fr_CA"}).hint({b: 1}).returnKey().next().b);
}

// Test that a query with a string comparison can use an index with a non-simple collation if it
// has a matching collation.
coll = testDb.collation_index3;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr_CA"}}));

// Query has simple collation, but index has fr_CA collation.
explainRes = coll.find({a: "foo"}).explain();
assert.commandWorked(explainRes);
explainRes = getSingleNodeExplain(explainRes);
assert(planHasStage(testDb, getWinningPlanFromExplain(explainRes.queryPlanner), "COLLSCAN"));

// Query has en_US collation, but index has fr_CA collation.
explainRes = coll.find({a: "foo"}).collation({locale: "en_US"}).explain();
assert.commandWorked(explainRes);
explainRes = getSingleNodeExplain(explainRes);
assert(planHasStage(testDb, getWinningPlanFromExplain(explainRes.queryPlanner), "COLLSCAN"));

// Matching collations.
explainRes = coll.find({a: "foo"}).collation({locale: "fr_CA"}).explain();
assert.commandWorked(explainRes);
explainRes = getSingleNodeExplain(explainRes);
assert(planHasStage(testDb, getWinningPlanFromExplain(explainRes.queryPlanner), "IXSCAN"));

// Should not be possible to create a text index with an explicit non-simple collation.
coll = testDb.collation_index3;
coll.drop();
assert.commandFailed(coll.createIndex({a: "text"}, {collation: {locale: "en"}}));

// Text index builds which inherit a non-simple default collation should fail.
coll = testDb.collation_en1;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en"}}));
assert.commandFailed(coll.createIndex({a: "text"}));

// Text index build should succeed on a collection with a non-simple default collation if it
// explicitly overrides the default with {locale: "simple"}.
coll = testDb.collation_en2;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en"}}));
assert.commandWorked(coll.createIndex({a: "text"}, {collation: {locale: "simple"}}));

//
// Collation tests for aggregation.
//

// Aggregation should return correct results when collation specified and collection does not
// exist.
coll = testDb.collation_agg1;
coll.drop();
assert.eq(0, coll.aggregate([], {collation: {locale: "fr"}}).itcount());

// Aggregation should return correct results when collation specified and collection does exist.
coll = testDb.collation_agg2;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "bar"}));
assert.eq(0, coll.aggregate([{$match: {str: "FOO"}}]).itcount());
assert.eq(1, coll.aggregate([{$match: {str: "FOO"}}], {collation: {locale: "en_US", strength: 2}}).itcount());

// Aggregation should return correct results when no collation specified and collection has a
// default collation.
coll = testDb.collation_agg3;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({str: "foo"}));
assert.eq(1, coll.aggregate([{$match: {str: "FOO"}}]).itcount());

// Aggregation should return correct results when "simple" collation specified and collection
// has a default collation.
coll = testDb.collation_agg4;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({str: "foo"}));
assert.eq(0, coll.aggregate([{$match: {str: "FOO"}}], {collation: {locale: "simple"}}).itcount());

// Aggregation should select compatible index when no collation specified and collection has a
// default collation.
coll = testDb.collation_agg5;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "en_US"}}));
var explain = coll.explain("queryPlanner").aggregate([{$match: {a: "foo"}}]);
explain = getSingleNodeExplain(explain);
assert(isIxscan(testDb, getWinningPlanFromExplain(explain.queryPlanner)));

// Aggregation should not use index when no collation specified and collection default
// collation is incompatible with index collation.
coll = testDb.collation_agg6;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "simple"}}));
var explain = coll.explain("queryPlanner").aggregate([{$match: {a: "foo"}}]);
explain = getSingleNodeExplain(explain);
assert(isCollscan(testDb, getWinningPlanFromExplain(explain.queryPlanner)));

// Explain of aggregation with collation should succeed.
assert.commandWorked(coll.explain().aggregate([], {collation: {locale: "fr"}}));

//
// Collation tests for count.
//

// Count should return correct results when collation specified and collection does not exist.
coll = testDb.collation_count1;
coll.drop();
assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).count());

// Count should return correct results when collation specified and collection does exist.
coll = testDb.collation_count1;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "bar"}));
assert.eq(0, coll.find({str: "FOO"}).count());
assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).count());
assert.eq(1, coll.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).count());
assert.eq(0, coll.count({str: "FOO"}));
assert.eq(0, coll.count({str: "FOO"}, {collation: {locale: "en_US"}}));
assert.eq(1, coll.count({str: "FOO"}, {collation: {locale: "en_US", strength: 2}}));

// Count should return correct results when no collation specified and collection has a default
// collation.
coll = testDb.collation_count3;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({str: "foo"}));
assert.eq(1, coll.find({str: "FOO"}).count());

// Count should return correct results when "simple" collation specified and collection has a
// default collation.
coll = testDb.collation_count4;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({str: "foo"}));
assert.eq(0, coll.find({str: "FOO"}).collation({locale: "simple"}).count());

// Count should return correct results when collation specified and when run with explain.
coll = testDb.collation_count5;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "bar"}));
explainRes = coll.explain("executionStats").find({str: "FOO"}).collation({locale: "en_US"}).count();
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "COLLSCAN");
assert.neq(null, planStage);
assert.eq(0, planStage.advanced);
explainRes = coll.explain("executionStats").find({str: "FOO"}).collation({locale: "en_US", strength: 2}).count();
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "COLLSCAN");
assert.neq(null, planStage);
assert.eq(1, planStage.advanced);

// Explain of COUNT_SCAN stage should include index collation.
coll = testDb.collation_count6;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "fr_CA"}}));
explainRes = coll.explain("executionStats").find({a: 5}).count();
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "COUNT_SCAN");
assert.neq(null, planStage);
assert.eq(planStage.collation, {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

// Explain of COUNT_SCAN stage should include index collation when index collation is
// inherited from collection default.
coll = testDb.collation_count7;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
assert.commandWorked(coll.createIndex({a: 1}));
explainRes = coll.explain("executionStats").find({a: 5}).count();
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "COUNT_SCAN");
assert.neq(null, planStage);
assert.eq(planStage.collation, {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

// Should be able to use COUNT_SCAN for queries over strings.
coll = testDb.collation_count8;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
assert.commandWorked(coll.createIndex({a: 1}));
explainRes = coll.explain("executionStats").find({a: "foo"}).count();
assert.commandWorked(explainRes);
assert(planHasStage(testDb, explainRes.executionStats.executionStages, "COUNT_SCAN"));
assert(!planHasStage(testDb, explainRes.executionStats.executionStages, "FETCH"));

//
// Collation tests for distinct.
//

// Distinct should return correct results when collation specified and collection does not
// exist.
coll = testDb.collation_distinct1;
coll.drop();
assert.eq(0, coll.distinct("str", {}, {collation: {locale: "en_US", strength: 2}}).length);

// Distinct should return correct results when collation specified and no indexes exist.
coll = testDb.collation_distinct2;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "FOO"}));
let res = coll.distinct("str", {}, {collation: {locale: "en_US", strength: 2}});
assert.eq(1, res.length);
assert.eq("foo", res[0].toLowerCase());
assert.eq(2, coll.distinct("str", {}, {collation: {locale: "en_US", strength: 3}}).length);
assert.eq(2, coll.distinct("_id", {str: "foo"}, {collation: {locale: "en_US", strength: 2}}).length);

// Distinct should return correct results when collation specified and compatible index exists.
coll.createIndex({str: 1}, {collation: {locale: "en_US", strength: 2}});
res = coll.distinct("str", {}, {collation: {locale: "en_US", strength: 2}});
assert.eq(1, res.length);
assert.eq("foo", res[0].toLowerCase());
assert.eq(2, coll.distinct("str", {}, {collation: {locale: "en_US", strength: 3}}).length);

// Distinct should return correct results when no collation specified and collection has a
// default collation.
coll = testDb.collation_distinct3;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({str: "foo"}));
assert.commandWorked(coll.insert({str: "FOO"}));
assert.eq(1, coll.distinct("str").length);
assert.eq(2, coll.distinct("_id", {str: "foo"}).length);

// Distinct should return correct results when "simple" collation specified and collection has a
// default collation.
coll = testDb.collation_distinct4;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({str: "foo"}));
assert.commandWorked(coll.insert({str: "FOO"}));
assert.eq(2, coll.distinct("str", {}, {collation: {locale: "simple"}}).length);
assert.eq(1, coll.distinct("_id", {str: "foo"}, {collation: {locale: "simple"}}).length);

// Distinct should select compatible index when no collation specified and collection has a
// default collation.
coll = testDb.collation_distinct5;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "en_US"}}));
var explain = coll.explain("queryPlanner").distinct("a");
assert(planHasStage(testDb, getWinningPlanFromExplain(explain.queryPlanner), "DISTINCT_SCAN"));
assert(distinctScanPlanHasFetch(getWinningPlanFromExplain(explain.queryPlanner)));

// Distinct scan on strings can be used over an index with a collation when the predicate has
// exact bounds.
explain = coll.explain("queryPlanner").distinct("a", {a: {$gt: "foo"}});
assert(planHasStage(testDb, getWinningPlanFromExplain(explain.queryPlanner), "DISTINCT_SCAN"));
assert(distinctScanPlanHasFetch(getWinningPlanFromExplain(explain.queryPlanner)));
assert(!planHasStage(testDb, getWinningPlanFromExplain(explain.queryPlanner), "PROJECTION_COVERED"));

// Distinct scan cannot be used over an index with a collation when the predicate has inexact
// bounds.
explain = coll.explain("queryPlanner").distinct("a", {a: {$exists: true}});
assert(planHasStage(testDb, getWinningPlanFromExplain(explain.queryPlanner), "IXSCAN"));
assert(planHasStage(testDb, getWinningPlanFromExplain(explain.queryPlanner), "FETCH"));
assert(!planHasStage(testDb, getWinningPlanFromExplain(explain.queryPlanner), "DISTINCT_SCAN"));

// Distinct scan can be used without a fetch when predicate has exact non-string bounds.
explain = coll.explain("queryPlanner").distinct("a", {a: {$gt: 3}});
assert(planHasStage(testDb, getWinningPlanFromExplain(explain.queryPlanner), "DISTINCT_SCAN"));
assert(planHasStage(testDb, getWinningPlanFromExplain(explain.queryPlanner), "PROJECTION_COVERED"));
assert(!distinctScanPlanHasFetch(getWinningPlanFromExplain(explain.queryPlanner)));

// Distinct should not use index when no collation specified and collection default collation is
// incompatible with index collation.
coll = testDb.collation_distinct6;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "simple"}}));
var explain = coll.explain("queryPlanner").distinct("a");
assert(isCollscan(testDb, getWinningPlanFromExplain(explain.queryPlanner)));

// Explain of DISTINCT_SCAN stage should include index collation.
coll = testDb.collation_distinct7;
coll.drop();
assert.commandWorked(coll.createIndex({str: 1}, {collation: {locale: "fr_CA"}}));
explainRes = coll.explain("executionStats").distinct("str", {}, {collation: {locale: "fr_CA"}});
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "DISTINCT_SCAN");
assert.neq(null, planStage);
assert.eq(planStage.collation, {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

// Explain of DISTINCT_SCAN stage should include index collation when index collation is
// inherited from collection default.
coll = testDb.collation_distinct8;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
assert.commandWorked(coll.createIndex({str: 1}));
explainRes = coll.explain("executionStats").distinct("str");
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "DISTINCT_SCAN");
assert.neq(null, planStage);
assert.eq(planStage.collation, {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

//
// Collation tests for find.
//

// Find should return correct results when collation specified and collection does not
// exist.
coll = testDb.collation_find1;
coll.drop();
assert.eq(0, coll.find({_id: "FOO"}).collation({locale: "en_US"}).itcount());

// Find should return correct results when collation specified and filter is a match on _id.
coll = testDb.collation_find2;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "bar"}));
assert.commandWorked(coll.insert({_id: "foo"}));
assert.eq(0, coll.find({_id: "FOO"}).itcount());
assert.eq(0, coll.find({_id: "FOO"}).collation({locale: "en_US"}).itcount());
assert.eq(1, coll.find({_id: "FOO"}).collation({locale: "en_US", strength: 2}).itcount());
assert.commandWorked(coll.remove({_id: "foo"}));

// Find should return correct results when collation specified and no indexes exist.
assert.eq(0, coll.find({str: "FOO"}).itcount());
assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).itcount());
assert.eq(1, coll.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).itcount());
assert.eq(
    1,
    coll
        .find({str: {$ne: "FOO"}})
        .collation({locale: "en_US", strength: 2})
        .itcount(),
);

// Find should return correct results when collation specified and compatible index exists.
assert.commandWorked(coll.createIndex({str: 1}, {collation: {locale: "en_US", strength: 2}}));
assert.eq(1, coll.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).hint({str: 1}).itcount());

// Find should return correct results even when the hinted index has a collation which does not
// match the command. This degrades to a whole ixscan followed by a fetch with residual filter to
// apply the collation-sensitive equality predicate.
assert.eq(0, coll.find({str: "FOO"}).hint({str: 1}).itcount());
assert.eq(0, coll.find({str: "FOO"}).collation({locale: "en_US"}).hint({str: 1}).itcount());
explainRes = coll.find({str: "FOO"}).hint({str: 1}).explain();
let ixscan = getPlanStage(getWinningPlanFromExplain(explainRes.queryPlanner), "IXSCAN");
assert.eq("str_1", ixscan.indexName);
assert.eq({str: ["[MinKey, MaxKey]"]}, ixscan.indexBounds);

assert.eq(
    1,
    coll
        .find({str: {$ne: "FOO"}})
        .collation({locale: "en_US", strength: 2})
        .hint({str: 1})
        .itcount(),
);
assert.commandWorked(coll.dropIndexes());

// Find should return correct results when collation specified and compatible partial index
// exists.
if (!TestData.isHintsToQuerySettingsSuite) {
    // This guard excludes this test case from being run on cursor_hints_to_query_settings suite.
    // The suite replaces cursor hints with query settings. Query settings do not force
    // partial/sparse indexes with incomplete result sets as described in SERVER-26413, and as such
    // will yield different results.
    assert.commandWorked(
        coll.createIndex(
            {str: 1},
            {
                partialFilterExpression: {str: {$lte: "FOO"}},
                collation: {locale: "en_US", strength: 2},
            },
        ),
    );
    assert.eq(1, coll.find({str: "foo"}).collation({locale: "en_US", strength: 2}).hint({str: 1}).itcount());
    assert.commandWorked(coll.insert({_id: 3, str: "goo"}));
    assert.eq(0, coll.find({str: "goo"}).collation({locale: "en_US", strength: 2}).hint({str: 1}).itcount());
    assert.commandWorked(coll.remove({_id: 3}));
    assert.commandWorked(coll.dropIndexes());
}

// Queries that use a index with a non-matching collation should add a sort
// stage if needed.
coll = testDb.collation_find3;
coll.drop();
assert.commandWorked(coll.insert([{a: "A"}, {a: "B"}, {a: "b"}, {a: "a"}]));

// Ensure results from an index that doesn't match the query collation are sorted to match
// the requested collation.
assert.commandWorked(coll.createIndex({a: 1}));
res = coll
    .find({a: {"$exists": true}}, {_id: 0})
    .collation({locale: "en_US", strength: 3})
    .sort({
        a: 1,
    });
assert.eq(res.toArray(), [{a: "a"}, {a: "A"}, {a: "b"}, {a: "B"}]);

// Hinting the incompatible index should not change the result, but we should still see an ixscan.
res = coll.find({}, {_id: 0}).collation({locale: "en_US", strength: 3}).sort({a: 1}).hint({a: 1});
assert.eq(res.toArray(), [{a: "a"}, {a: "A"}, {a: "b"}, {a: "B"}]);
res = coll.find({}, {_id: 0}).collation({locale: "en_US", strength: 3}).sort({a: 1}).hint({a: 1}).explain();
assert(isIxscan(testDb, getWinningPlanFromExplain(res.queryPlanner)));

// Find should return correct results when collation specified and query contains $expr.
coll = testDb.collation_find4;
coll.drop();
assert.commandWorked(coll.insert([{a: "A"}, {a: "B"}]));
assert.eq(
    1,
    coll
        .find({$expr: {$eq: ["$a", "a"]}})
        .collation({locale: "en_US", strength: 2})
        .itcount(),
);

// Find should return correct results when no collation specified and collection has a default
// collation.
coll = testDb.collation_find5;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({str: "foo"}));
assert.commandWorked(coll.insert({str: "FOO"}));
assert.commandWorked(coll.insert({str: "bar"}));
assert.eq(3, coll.find({str: {$in: ["foo", "bar"]}}).itcount());
assert.eq(2, coll.find({str: "foo"}).itcount());
assert.eq(1, coll.find({str: {$ne: "foo"}}).itcount());
assert.eq([{str: "bar"}, {str: "foo"}, {str: "FOO"}], coll.find({}, {_id: 0, str: 1}).sort({str: 1}).toArray());

// Find should return correct results when hinting an index which has a collation that matches the
// collection default.
assert.commandWorked(coll.createIndex({str: 1}, {collation: {locale: "en_US", strength: 2}}));
assert.eq(
    3,
    coll
        .find({str: {$in: ["foo", "bar"]}})
        .hint({str: 1})
        .itcount(),
);
assert.eq(2, coll.find({str: "foo"}).hint({str: 1}).itcount());
assert.eq(
    1,
    coll
        .find({str: {$ne: "foo"}})
        .hint({str: 1})
        .itcount(),
);

// Find should return correct results when hinting an index which has a different collation than the
// collection default.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({str: 1}, {collation: {locale: "simple"}}));
assert.eq(
    3,
    coll
        .find({str: {$in: ["foo", "bar"]}})
        .hint({str: 1})
        .itcount(),
);
assert.eq(2, coll.find({str: "foo"}).hint({str: 1}).itcount());
assert.eq(
    1,
    coll
        .find({str: {$ne: "foo"}})
        .hint({str: 1})
        .itcount(),
);

// Find with idhack should return correct results when no collation specified and collection has
// a default collation.
coll = testDb.collation_find6;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: "foo"}));
assert.eq(1, coll.find({_id: "FOO"}).itcount());

// Find should return correct results for query containing $expr when no collation specified and
// collection has a default collation.
coll = testDb.collation_find7;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert([{a: "A"}, {a: "B"}]));
assert.eq(1, coll.find({$expr: {$eq: ["$a", "a"]}}).itcount());

// Find should return correct results when "simple" collation specified and collection has a
// default collation.
coll = testDb.collation_find8;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({str: "foo"}));
assert.commandWorked(coll.insert({str: "FOO"}));
assert.commandWorked(coll.insert({str: "bar"}));
assert.eq(
    2,
    coll
        .find({str: {$in: ["foo", "bar"]}})
        .collation({locale: "simple"})
        .itcount(),
);
assert.eq(1, coll.find({str: "foo"}).collation({locale: "simple"}).itcount());
assert.eq(
    [{str: "FOO"}, {str: "bar"}, {str: "foo"}],
    coll.find({}, {_id: 0, str: 1}).sort({str: 1}).collation({locale: "simple"}).toArray(),
);

// Find on _id should return correct results when query collation differs from collection
// default collation.
coll = testDb.collation_find9;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 3}}));
assert.commandWorked(coll.insert({_id: "foo"}));
assert.commandWorked(coll.insert({_id: "FOO"}));
assert.eq(2, coll.find({_id: "foo"}).collation({locale: "en_US", strength: 2}).itcount());

if (!isClustered) {
    // Find on _id should use fast path when query inherits collection default collation.
    coll = testDb.collation_find10;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").find({_id: "foo"}).finish();
    assert.commandWorked(explainRes);
    assert(isIdhackOrExpress(testDb, getWinningPlanFromExplain(explainRes.queryPlanner)));

    // Find on _id should use fastpath when explicitly given query collation matches
    // collection default.
    coll = testDb.collation_find11;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").find({_id: "foo"}).collation({locale: "en_US"}).finish();
    assert.commandWorked(explainRes);
    assert(isIdhackOrExpress(testDb, getWinningPlanFromExplain(explainRes.queryPlanner)));

    // Find on _id should not use fastpath when query collation does not match
    // collection default.
    coll = testDb.collation_find12;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").find({_id: "foo"}).collation({locale: "fr_CA"}).finish();
    assert.commandWorked(explainRes);
    assert(!isIdhackOrExpress(testDb, getWinningPlanFromExplain(explainRes.queryPlanner)));
}

// Find should select compatible index when no collation specified and collection has a default
// collation.
coll = testDb.collation_find13;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "en_US"}}));
var explain = coll.find({a: "foo"}).explain("queryPlanner");

// Assert the plan is using an index scan.
assert(isIxscan(testDb, getWinningPlanFromExplain(explain.queryPlanner)));

// Find should select compatible index when no collation specified and collection default
// collation is "simple".
coll = testDb.collation_find14;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "simple"}}));
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "simple"}}));
var explain = coll.find({a: "foo"}).explain("queryPlanner");

// Assert the plan is using an index scan.
assert(isIxscan(testDb, getWinningPlanFromExplain(explain.queryPlanner)));

// Find should not use index when no collation specified, index collation is "simple", and
// collection has a non-"simple" default collation.
coll = testDb.collation_find15;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "simple"}}));
var explain = coll.find({a: "foo"}).explain("queryPlanner");
assert(isCollscan(testDb, getWinningPlanFromExplain(explain.queryPlanner)));

// Find should select compatible index when "simple" collation specified and collection has a
// non-"simple" default collation.
coll = testDb.collation_find16;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
assert.commandWorked(coll.createIndex({a: 1}, {collation: {locale: "simple"}}));
var explain = coll.find({a: "foo"}).collation({locale: "simple"}).explain("queryPlanner");
assert(isIxscan(testDb, getWinningPlanFromExplain(explain.queryPlanner)));

// Find should return correct results when collation specified and run with explain.
coll = testDb.collation_find17;
coll.drop();
assert.commandWorked(coll.insert({str: "foo"}));
explainRes = coll.explain("executionStats").find({str: "FOO"}).collation({locale: "en_US"}).finish();
assert.commandWorked(explainRes);
assert.eq(0, explainRes.executionStats.nReturned);
explainRes = coll.explain("executionStats").find({str: "FOO"}).collation({locale: "en_US", strength: 2}).finish();
assert.commandWorked(explainRes);
assert.eq(1, explainRes.executionStats.nReturned);

// Explain of find should include query collation.
coll = testDb.collation_find18;
coll.drop();
explainRes = coll.explain("executionStats").find({str: "foo"}).collation({locale: "fr_CA"}).finish();
assert.commandWorked(explainRes);
assert.eq(getQueryCollation(explainRes), {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

// Explain of find should include query collation when inherited from collection default.
coll = testDb.collation_find19;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
explainRes = coll.explain("executionStats").find({str: "foo"}).finish();
assert.commandWorked(explainRes);
assert.eq(getQueryCollation(explainRes), {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

// Explain of IXSCAN stage should include index collation.
coll = testDb.collation_find20;
coll.drop();
assert.commandWorked(coll.createIndex({str: 1}, {collation: {locale: "fr_CA"}}));
explainRes = coll.explain("executionStats").find({str: "foo"}).collation({locale: "fr_CA"}).finish();
assert.commandWorked(explainRes);
planStage = getPlanStage(getWinningPlanFromExplain(explainRes.queryPlanner), "IXSCAN");
assert.neq(null, planStage);
assert.eq(planStage.collation, {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

// Explain of IXSCAN stage should include index collation when index collation is inherited from
// collection default.
coll = testDb.collation_find21;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "fr_CA"}}));
assert.commandWorked(coll.createIndex({str: 1}));
explainRes = coll.explain("executionStats").find({str: "foo"}).finish();
assert.commandWorked(explainRes);
planStage = getPlanStage(getWinningPlanFromExplain(explainRes.queryPlanner), "IXSCAN");
assert.neq(null, planStage);
assert.eq(planStage.collation, {
    locale: "fr_CA",
    caseLevel: false,
    caseFirst: "off",
    strength: 3,
    numericOrdering: false,
    alternate: "non-ignorable",
    maxVariable: "punct",
    normalization: false,
    backwards: true,
    version: "57.1",
});

// Queries that have an index with a matching collation should return correctly ordered results.
coll = testDb.collation_find22;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en", strength: 2}}));
assert.commandWorked(coll.createIndex({x: 1, y: 1, z: 1}));
for (let i = 0; i < 10; ++i) {
    assert.commandWorked(coll.insert({_id: i, x: 1, y: 10 - i, z: "str" + (i % 3)}));
}
const expectedResultsAsc = [{"z": "str0"}, {"z": "str0"}, {"z": "str1"}, {"z": "str2"}];
assert.eq(
    expectedResultsAsc,
    coll
        .find({y: {$lt: 5}}, {_id: 0, z: 1})
        .sort({z: 1})
        .toArray(),
);
const expectedResultsDesc = [{"z": "str2"}, {"z": "str1"}, {"z": "str0"}, {"z": "str0"}];
assert.eq(
    expectedResultsDesc,
    coll
        .find({y: {$lt: 5}}, {_id: 0, z: 1})
        .sort({z: -1})
        .toArray(),
);

//
// Collation tests for findAndModify.
//

// findAndModify should return correct results when collation specified and collection does not
// exist.
coll = testDb.collation_findmodify1;
coll.drop();
assert.eq(
    null,
    coll.findAndModify({query: {str: "bar"}, update: {$set: {str: "baz"}}, new: true, collation: {locale: "fr"}}),
);

// Update-findAndModify should return correct results when collation specified.
coll = testDb.collation_findmodify2;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "bar"}));
assert.eq(
    {_id: 1, str: "baz"},
    coll.findAndModify({
        query: {str: "FOO"},
        update: {$set: {str: "baz"}},
        new: true,
        collation: {locale: "en_US", strength: 2},
    }),
);

// Explain of update-findAndModify should return correct results when collation specified.
explainRes = coll.explain("executionStats").findAndModify({
    query: {str: "BAR"},
    update: {$set: {str: "baz"}},
    new: true,
    collation: {locale: "en_US", strength: 2},
});
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "UPDATE");
assert.neq(null, planStage);
assert.eq(1, planStage.nWouldModify);

// Delete-findAndModify should return correct results when collation specified.
coll = testDb.collation_findmodify3;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "bar"}));
assert.eq(
    {_id: 1, str: "foo"},
    coll.findAndModify({query: {str: "FOO"}, remove: true, collation: {locale: "en_US", strength: 2}}),
);

// Explain of delete-findAndModify should return correct results when collation specified.
explainRes = coll.explain("executionStats").findAndModify({
    query: {str: "BAR"},
    remove: true,
    collation: {locale: "en_US", strength: 2},
});
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "DELETE");
assert.neq(null, planStage);
assert.eq(1, planStage.nWouldDelete);

// findAndModify should return correct results when no collation specified and collection has a
// default collation.
coll = testDb.collation_findmodify4;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.eq({_id: 1, str: "foo"}, coll.findAndModify({query: {str: "FOO"}, update: {$set: {x: 1}}}));

// Case of _id lookup and projection on a collection with collection-default collation. Note that
// retryable writes do not always respect the 'fields' option (SERVER-31242) so we must include
// all fields in the document.
assert.eq(
    {_id: 1, str: "foo", x: 1},
    coll.findAndModify({query: {_id: 1}, update: {$inc: {x: 1}}, fields: {str: 1, x: 1}}),
);
// Case of _id lookup and hint on a collection with collection-default collation.
assert.commandWorked(coll.createIndex({x: 1}));
assert.eq({_id: 1, str: "foo", x: 2}, coll.findAndModify({query: {_id: 1}, update: {$inc: {x: 1}}, hint: {x: 1}}));
assert.eq({_id: 1, str: "foo", x: 3}, coll.findAndModify({query: {_id: 1}, update: {$inc: {x: 1}}, hint: {_id: 1}}));

// Remove the document.
assert.eq({_id: 1, str: "foo", x: 4}, coll.findAndModify({query: {str: "FOO"}, remove: true}));

// findAndModify should return correct results when "simple" collation specified and collection
// has a default collation.
coll = testDb.collation_findmodify5;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.eq(null, coll.findAndModify({query: {str: "FOO"}, update: {$set: {x: 1}}, collation: {locale: "simple"}}));
assert.eq(null, coll.findAndModify({query: {str: "FOO"}, remove: true, collation: {locale: "simple"}}));

//
// Collation tests for mapReduce.
//

// mapReduce should return correct results when collation specified and no indexes exist.
coll = testDb.collation_mapreduce1;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "bar"}));
var mapReduceOut = coll.mapReduce(
    function () {
        emit(this.str, 1);
    },
    function (key, values) {
        return Array.sum(values);
    },
    {out: {inline: 1}, query: {str: "FOO"}, collation: {locale: "en_US", strength: 2}},
);
assert.commandWorked(mapReduceOut);
assert.eq(mapReduceOut.results.length, 1);

// mapReduce should return correct results when no collation specified and collection has a
// default collation.
coll = testDb.collation_mapreduce2;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
var mapReduceOut = coll.mapReduce(
    function () {
        emit(this.str, 1);
    },
    function (key, values) {
        return Array.sum(values);
    },
    {out: {inline: 1}, query: {str: "FOO"}},
);
assert.commandWorked(mapReduceOut);
assert.eq(mapReduceOut.results.length, 1);

// mapReduce should return correct results when "simple" collation specified and collection has
// a default collation.
coll = testDb.collation_mapreduce3;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
var mapReduceOut = coll.mapReduce(
    function () {
        emit(this.str, 1);
    },
    function (key, values) {
        return Array.sum(values);
    },
    {out: {inline: 1}, query: {str: "FOO"}, collation: {locale: "simple"}},
);
assert.commandWorked(mapReduceOut);
assert.eq(mapReduceOut.results.length, 0);

// mapReduce should correctly combine results when no collation is specified and the collection has
// a default collation.
coll = testDb.collation_mapreduce4;
const outCollName = coll.getName() + "_outcoll";
const outColl = testDb[outCollName];
coll.drop();
outColl.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(testDb.createCollection(outColl.getName()));
assert.commandWorked(coll.insert({_id: 1, str: "foo", amt: 100}));
assert.commandWorked(coll.insert({_id: 2, str: "FOO", amt: 200}));
assert.commandWorked(coll.insert({_id: 3, str: "foo", amt: 300}));
assert.commandWorked(coll.insert({_id: 4, str: "FOO", amt: 400}));
assert.commandWorked(
    coll.mapReduce(
        function () {
            emit(this.str, this.amt);
        },
        function (key, values) {
            return Array.sum(values);
        },
        {out: {reduce: outCollName}},
    ),
);

// Using the case insensitive collation should leave us with a single result.
const mrResult = outColl.find().toArray();
assert.eq(mrResult.length, 1, mrResult);
assert.eq(mrResult[0].value, 1000, mrResult);

//
// Collation tests for remove.
//

// Remove should succeed when collation specified and collection does not exist.
coll = testDb.collation_remove1;
coll.drop();
assert.commandWorked(coll.remove({str: "foo"}, {justOne: true, collation: {locale: "fr"}}));

// Remove should return correct results when collation specified.
coll = testDb.collation_remove2;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
writeRes = coll.remove({str: "FOO"}, {justOne: true, collation: {locale: "en_US", strength: 2}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// Explain of remove should return correct results when collation specified.
coll = testDb.collation_remove3;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
explainRes = coll
    .explain("executionStats")
    .remove({str: "FOO"}, {justOne: true, collation: {locale: "en_US", strength: 2}});
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "DELETE");
assert.neq(null, planStage);
assert.eq(1, planStage.nWouldDelete);

// Remove should return correct results when no collation specified and collection has a default
// collation.
coll = testDb.collation_remove4;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
writeRes = coll.remove({str: "FOO"}, {justOne: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// Remove with idhack should return correct results when no collation specified and collection
// has a default collation.
coll = testDb.collation_remove5;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: "foo"}));
writeRes = coll.remove({_id: "FOO"}, {justOne: true});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// Remove should return correct results for an IDHACK eligible query when no collation is specified
// but a hint is specified.
assert.commandWorked(coll.insert({_id: "foo"}));
assert.commandWorked(coll.createIndex({a: 1}));
writeRes = coll.remove({_id: "FOO"}, {justOne: true, hint: {a: 1}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

if (!isClustered) {
    // Remove on _id should use idhack stage when query inherits collection default collation.
    coll = testDb.collation_remove6;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").remove({_id: "foo"});
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    if (planStage == null) {
        // post 8.0 EXPRESS handles delete-by-id
        planStage = getPlanStage(explainRes.executionStats.executionStages, "EXPRESS_DELETE");
    }
    assert.neq(null, planStage);
}

// Remove should return correct results when "simple" collation specified and collection has
// a default collation.
coll = testDb.collation_remove7;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
writeRes = coll.remove({str: "FOO"}, {justOne: true, collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(0, writeRes.nRemoved);

// Remove on _id should return correct results when "simple" collation specified and
// collection has a default collation.
coll = testDb.collation_remove8;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: "foo"}));
writeRes = coll.remove({_id: "FOO"}, {justOne: true, collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(0, writeRes.nRemoved);

if (!isClustered) {
    // Remove on _id should use idhack stage when explicit query collation matches collection
    // default.
    coll = testDb.collation_remove9;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").remove({_id: "foo"}, {collation: {locale: "en_US"}});
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    if (planStage == null) {
        // post 8.0 EXPRESS handles delete-by-id
        planStage = getPlanStage(explainRes.executionStats.executionStages, "EXPRESS_DELETE");
    }
    assert.neq(null, planStage);

    // Remove on _id should not use express stage when query collation does not match collection
    // default.
    coll = testDb.collation_remove10;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").remove({_id: "foo"}, {collation: {locale: "fr_CA"}});
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    assert.eq(null, planStage);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "EXPRESS_DELETE");
    assert.eq(null, planStage);
}

//
// Collation tests for update.
//

// Update should succeed when collation specified and collection does not exist.
coll = testDb.collation_update1;
coll.drop();
assert.commandWorked(coll.update({str: "foo"}, {$set: {other: 99}}, {multi: true, collation: {locale: "fr"}}));

// Update should return correct results when collation specified.
coll = testDb.collation_update2;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
writeRes = coll.update({str: "FOO"}, {$set: {other: 99}}, {multi: true, collation: {locale: "en_US", strength: 2}});
assert.eq(2, writeRes.nModified);

// Explain of update should return correct results when collation specified.
coll = testDb.collation_update3;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
explainRes = coll.explain("executionStats").update(
    {str: "FOO"},
    {$set: {other: 99}},
    {
        multi: true,
        collation: {locale: "en_US", strength: 2},
    },
);
assert.commandWorked(explainRes);
planStage = getPlanStage(explainRes.executionStats.executionStages, "UPDATE");
assert.neq(null, planStage);
assert.eq(2, planStage.nWouldModify);

// Update should return correct results when no collation specified and collection has a default
// collation.
coll = testDb.collation_update4;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
writeRes = coll.update({str: "FOO"}, {$set: {other: 99}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);

// Update with idhack should return correct results when no collation specified and collection
// has a default collation.
coll = testDb.collation_update5;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: "foo"}));
writeRes = coll.update({_id: "FOO"}, {$set: {other: 99}});
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nMatched);

if (!isClustered) {
    // Update on _id should use idhack stage when query inherits collection default collation.
    coll = testDb.collation_update6;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").update({_id: "foo"}, {$set: {other: 99}});
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    if (planStage == null) {
        // post 8.0 EXPRESS handles update-by-id
        planStage = getPlanStage(explainRes.executionStats.executionStages, "EXPRESS_UPDATE");
    }
    assert.neq(null, planStage);
}

// Update should return correct results when "simple" collation specified and collection has
// a default collation.
coll = testDb.collation_update7;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
writeRes = coll.update({str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(0, writeRes.nModified);

// Update on _id should return correct results when "simple" collation specified and
// collection has a default collation.
coll = testDb.collation_update8;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.insert({_id: "foo"}));
writeRes = coll.update({_id: "FOO"}, {$set: {other: 99}}, {collation: {locale: "simple"}});
assert.commandWorked(writeRes);
assert.eq(0, writeRes.nModified);

if (!isClustered) {
    // Update on _id should use idhack stage when explicitly given query collation matches
    // collection default.
    coll = testDb.collation_update9;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").update(
        {_id: "foo"},
        {$set: {other: 99}},
        {
            collation: {locale: "en_US"},
        },
    );
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    if (planStage == null) {
        // post 8.0 EXPRESS handles update-by-id
        planStage = getPlanStage(explainRes.executionStats.executionStages, "EXPRESS_UPDATE");
    }
    assert.neq(null, planStage);

    // Update on _id should not use idhack stage when query collation does not match collection
    // default.
    coll = testDb.collation_update10;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US"}}));
    explainRes = coll.explain("executionStats").update(
        {_id: "foo"},
        {$set: {other: 99}},
        {
            collation: {locale: "fr_CA"},
        },
    );
    assert.commandWorked(explainRes);
    planStage = getPlanStage(explainRes.executionStats.executionStages, "IDHACK");
    if (planStage == null) {
        // post 8.0 EXPRESS handles update-by-id
        planStage = getPlanStage(explainRes.executionStats.executionStages, "EXPRESS_UPDATE");
    }
    assert.eq(null, planStage);
}

//
// Collation tests for the $geoNear aggregation stage.
//

// $geoNear should fail when collation is specified but the collection does not exist.
coll = testDb.collation_geonear1;
coll.drop();
assert.commandFailedWithCode(
    testDb.runCommand({
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "dist",
                },
            },
        ],
        collation: {locale: "en_US", strength: 2},
    }),
    ErrorCodes.NamespaceNotFound,
);

// $geoNear rejects the now-deprecated "collation" option.
coll = testDb.collation_geonear2;
coll.drop();
assert.commandWorked(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));
assert.commandFailedWithCode(
    testDb.runCommand({
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [
            {
                $geoNear: {
                    near: {type: "Point", coordinates: [0, 0]},
                    distanceField: "dist",
                    collation: {locale: "en_US"},
                },
            },
        ],
    }),
    40227,
);

const geoNearStage = {
    $geoNear: {
        near: {type: "Point", coordinates: [0, 0]},
        distanceField: "dist",
        spherical: true,
        query: {str: "ABC"},
    },
};

// $geoNear should return correct results when collation specified and string predicate not
// indexed.
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
assert.eq(0, coll.aggregate([geoNearStage]).itcount());
assert.eq(1, coll.aggregate([geoNearStage], {collation: {locale: "en_US", strength: 2}}).itcount());

// $geoNear should return correct results when no collation specified and string predicate
// indexed.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({geo: "2dsphere", str: 1}));
assert.eq(0, coll.aggregate([geoNearStage]).itcount());
assert.eq(1, coll.aggregate([geoNearStage], {collation: {locale: "en_US", strength: 2}}).itcount());

// $geoNear should return correct results when collation specified and collation on index is
// incompatible with string predicate.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({geo: "2dsphere", str: 1}, {collation: {locale: "en_US", strength: 3}}));
assert.eq(0, coll.aggregate([geoNearStage]).itcount());
assert.eq(1, coll.aggregate([geoNearStage], {collation: {locale: "en_US", strength: 2}}).itcount());

// $geoNear should return correct results when collation specified and collation on index is
// compatible with string predicate.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({geo: "2dsphere", str: 1}, {collation: {locale: "en_US", strength: 2}}));
assert.eq(0, coll.aggregate([geoNearStage]).itcount());
assert.eq(1, coll.aggregate([geoNearStage], {collation: {locale: "en_US", strength: 2}}).itcount());

// $geoNear should return correct results when no collation specified and collection has a
// default collation.
coll = testDb.collation_geonear3;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
assert.commandWorked(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));
assert.eq(1, coll.aggregate([geoNearStage]).itcount());

// $geoNear should return correct results when "simple" collation specified and collection has
// a default collation.
coll = testDb.collation_geonear4;
coll.drop();
assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
assert.commandWorked(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));
assert.eq(0, coll.aggregate([geoNearStage], {collation: {locale: "simple"}}).itcount());

//
// Collation tests for find with $nearSphere.
//

// Find with $nearSphere should return correct results when collation specified and
// collection does not exist.
coll = testDb.collation_nearsphere1;
coll.drop();
assert.eq(
    0,
    coll
        .find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}})
        .collation({locale: "en_US", strength: 2})
        .itcount(),
);

// Find with $nearSphere should return correct results when collation specified and string
// predicate not indexed.
coll = testDb.collation_nearsphere2;
coll.drop();
assert.commandWorked(coll.insert({geo: {type: "Point", coordinates: [0, 0]}, str: "abc"}));
assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
assert.eq(0, coll.find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}}).itcount());
assert.eq(
    1,
    coll
        .find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}})
        .collation({locale: "en_US", strength: 2})
        .itcount(),
);

// Find with $nearSphere should return correct results when no collation specified and
// string predicate indexed.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({geo: "2dsphere", str: 1}));
assert.eq(0, coll.find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}}).itcount());
assert.eq(
    1,
    coll
        .find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}})
        .collation({locale: "en_US", strength: 2})
        .itcount(),
);

// Find with $nearSphere should return correct results when collation specified and
// collation on index is incompatible with string predicate.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({geo: "2dsphere", str: 1}, {collation: {locale: "en_US", strength: 3}}));
assert.eq(0, coll.find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}}).itcount());
assert.eq(
    1,
    coll
        .find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}})
        .collation({locale: "en_US", strength: 2})
        .itcount(),
);

// Find with $nearSphere should return correct results when collation specified and
// collation on index is compatible with string predicate.
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({geo: "2dsphere", str: 1}, {collation: {locale: "en_US", strength: 2}}));
assert.eq(0, coll.find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}}).itcount());
assert.eq(
    1,
    coll
        .find({str: "ABC", geo: {$nearSphere: {$geometry: {type: "Point", coordinates: [0, 0]}}}})
        .collation({locale: "en_US", strength: 2})
        .itcount(),
);

//
// Tests for the bulk API.
//

let bulk;

// update().
coll = testDb.collation_bulkupdate;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
bulk = coll.initializeUnorderedBulkOp();
bulk.find({str: "FOO"})
    .collation({locale: "en_US", strength: 2})
    .update({$set: {other: 99}});
writeRes = bulk.execute();
assert.commandWorked(writeRes);
assert.eq(2, writeRes.nModified);

// updateOne().
coll = testDb.collation_bulkupdateone;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
bulk = coll.initializeUnorderedBulkOp();
bulk.find({str: "FOO"})
    .collation({locale: "en_US", strength: 2})
    .updateOne({$set: {other: 99}});
writeRes = bulk.execute();
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nModified);

// replaceOne().
coll = testDb.collation_bulkreplaceone1;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
bulk = coll.initializeUnorderedBulkOp();
bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).replaceOne({str: "oof"});
writeRes = bulk.execute();
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nModified);

// replaceOne() with upsert().
coll = testDb.collation_bulkreplaceone2;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
bulk = coll.initializeUnorderedBulkOp();
bulk.find({str: "FOO"}).collation({locale: "en_US"}).upsert().replaceOne({str: "foo"});
writeRes = bulk.execute();
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nUpserted);
assert.eq(0, writeRes.nModified);

bulk = coll.initializeUnorderedBulkOp();
bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).upsert().replaceOne({str: "foo"});
writeRes = bulk.execute();
assert.commandWorked(writeRes);
assert.eq(0, writeRes.nUpserted);
assert.eq(1, writeRes.nModified);

// removeOne().
coll = testDb.collation_bulkremoveone;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
bulk = coll.initializeUnorderedBulkOp();
bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).removeOne();
writeRes = bulk.execute();
assert.commandWorked(writeRes);
assert.eq(1, writeRes.nRemoved);

// remove().
coll = testDb.collation_bulkremove;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
bulk = coll.initializeUnorderedBulkOp();
bulk.find({str: "FOO"}).collation({locale: "en_US", strength: 2}).remove();
writeRes = bulk.execute();
assert.commandWorked(writeRes);
assert.eq(2, writeRes.nRemoved);

//
// Tests for the CRUD API.
//

// deleteOne().
coll = testDb.collation_deleteone;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.deleteOne({str: "FOO"}, {collation: {locale: "en_US", strength: 2}});
assert.eq(1, res.deletedCount);

// deleteMany().
coll = testDb.collation_deletemany;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.deleteMany({str: "FOO"}, {collation: {locale: "en_US", strength: 2}});
assert.eq(2, res.deletedCount);

// findOneAndDelete().
coll = testDb.collation_findonedelete;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.eq({_id: 1, str: "foo"}, coll.findOneAndDelete({str: "FOO"}, {collation: {locale: "en_US", strength: 2}}));
assert.eq(null, coll.findOne({_id: 1}));

// findOneAndReplace().
coll = testDb.collation_findonereplace;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.eq(
    {_id: 1, str: "foo"},
    coll.findOneAndReplace({str: "FOO"}, {str: "bar"}, {collation: {locale: "en_US", strength: 2}}),
);
assert.neq(null, coll.findOne({str: "bar"}));

// findOneAndUpdate().
coll = testDb.collation_findoneupdate;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.eq(
    {_id: 1, str: "foo"},
    coll.findOneAndUpdate({str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "en_US", strength: 2}}),
);
assert.neq(null, coll.findOne({other: 99}));

// replaceOne().
coll = testDb.collation_replaceone;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.replaceOne({str: "FOO"}, {str: "bar"}, {collation: {locale: "en_US", strength: 2}});
assert.eq(1, res.modifiedCount);

// updateOne().
coll = testDb.collation_updateone;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.updateOne({str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "en_US", strength: 2}});
assert.eq(1, res.modifiedCount);

// updateMany().
coll = testDb.collation_updatemany;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.updateMany({str: "FOO"}, {$set: {other: 99}}, {collation: {locale: "en_US", strength: 2}});
assert.eq(2, res.modifiedCount);

// updateOne with bulkWrite().
coll = testDb.collation_updateonebulkwrite;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.bulkWrite([
    {
        updateOne: {
            filter: {str: "FOO"},
            update: {$set: {other: 99}},
            collation: {locale: "en_US", strength: 2},
        },
    },
]);
assert.eq(1, res.matchedCount);

// updateOne with undefined/null collation.backwards parameter (SERVER-54482).
for (let backwards of [undefined, null]) {
    assert.throws(function () {
        coll.bulkWrite([
            {
                updateOne: {
                    filter: {str: "foo"},
                    update: {$set: {str: "bar"}},
                    collation: {locale: "en_US", backwards: backwards},
                },
            },
        ]);
    });
}

// updateMany with bulkWrite().
coll = testDb.collation_updatemanybulkwrite;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.bulkWrite([
    {
        updateMany: {
            filter: {str: "FOO"},
            update: {$set: {other: 99}},
            collation: {locale: "en_US", strength: 2},
        },
    },
]);
assert.eq(2, res.matchedCount);

// replaceOne with bulkWrite().
coll = testDb.collation_replaceonebulkwrite;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.bulkWrite([
    {
        replaceOne: {filter: {str: "FOO"}, replacement: {str: "bar"}, collation: {locale: "en_US", strength: 2}},
    },
]);
assert.eq(1, res.matchedCount);

// deleteOne with bulkWrite().
coll = testDb.collation_deleteonebulkwrite;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.bulkWrite([{deleteOne: {filter: {str: "FOO"}, collation: {locale: "en_US", strength: 2}}}]);
assert.eq(1, res.deletedCount);

// deleteMany with bulkWrite().
coll = testDb.collation_deletemanybulkwrite;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "foo"}));
res = coll.bulkWrite([{deleteMany: {filter: {str: "FOO"}, collation: {locale: "en_US", strength: 2}}}]);
assert.eq(2, res.deletedCount);

// Two deleteOne ops with bulkWrite using different collations.
coll = testDb.collation_deleteone2collation;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, str: "foo"}));
assert.commandWorked(coll.insert({_id: 2, str: "bar"}));
res = coll.bulkWrite([
    {deleteOne: {filter: {str: "FOO"}, collation: {locale: "fr", strength: 2}}},
    {deleteOne: {filter: {str: "BAR"}, collation: {locale: "en_US", strength: 2}}},
]);
assert.eq(2, res.deletedCount);

// applyOps.
if (!isMongos) {
    coll = testDb.collation_applyops;
    coll.drop();
    assert.commandWorked(testDb.createCollection(coll.getName(), {collation: {locale: "en_US", strength: 2}}));
    assert.commandWorked(coll.insert({_id: "foo", x: 5, str: "bar"}));

    // <operation>.o2 respects collection default collation.
    assert.commandWorked(
        testDb.runCommand({
            applyOps: [{op: "u", ns: coll.getFullName(), o2: {_id: "FOO"}, o: {$v: 2, diff: {u: {x: 6}}}}],
        }),
    );
    assert.eq(6, coll.findOne({_id: "foo"}).x);
}

// Test that the collection created with the "cloneCollectionAsCapped" command inherits the
// default collation of the corresponding collection. We skip running this command in a sharded
// cluster because it isn't supported by mongos.
// TODO SERVER-85773: Enale below test for sharded clusters.
if (!isMongos) {
    const clonedColl = testDb.collation_cloned;

    coll = testDb.collation_orig;
    coll.drop();
    clonedColl.drop();

    // Create a collection with a non-simple default collation.
    assert.commandWorked(testDb.runCommand({create: coll.getName(), collation: {locale: "en", strength: 2}}));
    const originalCollectionInfos = testDb.getCollectionInfos({name: coll.getName()});
    assert.eq(originalCollectionInfos.length, 1, tojson(originalCollectionInfos));

    assert.commandWorked(coll.insert({_id: "FOO"}));
    assert.commandWorked(coll.insert({_id: "bar"}));
    assert.eq(
        [{_id: "FOO"}],
        coll.find({_id: "foo"}).toArray(),
        "query should have performed a case-insensitive match",
    );

    let cloneCollOutput = testDb.runCommand({
        cloneCollectionAsCapped: coll.getName(),
        toCollection: clonedColl.getName(),
        size: 4096,
    });
    assert.commandWorked(cloneCollOutput);
    const clonedCollectionInfos = testDb.getCollectionInfos({name: clonedColl.getName()});
    assert.eq(clonedCollectionInfos.length, 1, tojson(clonedCollectionInfos));
    assert.eq(originalCollectionInfos[0].options.collation, clonedCollectionInfos[0].options.collation);
    assert.eq([{_id: "FOO"}], clonedColl.find({_id: "foo"}).toArray());
}

// Test that the find command's min/max options respect the collation.
coll = testDb.collation_minmax1;
coll.drop();
assert.commandWorked(coll.insert({str: "a"}));
assert.commandWorked(coll.insert({str: "A"}));
assert.commandWorked(coll.insert({str: "b"}));
assert.commandWorked(coll.insert({str: "B"}));
assert.commandWorked(coll.insert({str: "c"}));
assert.commandWorked(coll.insert({str: "C"}));
assert.commandWorked(coll.insert({str: "d"}));
assert.commandWorked(coll.insert({str: "D"}));

// This query should fail, since there is no index to support the min/max.
let err = assert.throws(() =>
    coll.find().min({str: "b"}).max({str: "D"}).collation({locale: "en_US", strength: 2}).itcount(),
);
assert.commandFailedWithCode(err, [ErrorCodes.NoQueryExecutionPlans, 51173]);

// Even after building an index with the right key pattern, the query should fail since the
// collations don't match.
assert.commandWorked(coll.createIndex({str: 1}, {name: "noCollation"}));
err = assert.throws(() =>
    coll.find().min({str: "b"}).max({str: "D"}).collation({locale: "en_US", strength: 2}).hint({str: 1}).itcount(),
);
assert.commandFailedWithCode(err, 51174);

// This query should fail, because the hinted index does not match the requested
// collation, and the 'max' value is a string, which means we cannot ignore the
// collation.
const caseInsensitive = {
    locale: "en",
    strength: 2,
};
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.createIndex({str: 1}));
err = assert.throws(() =>
    coll.find({}, {_id: 0}).min({str: MinKey}).max({str: "Hello1"}).hint({str: 1}).collation(caseInsensitive).toArray(),
);
assert.commandFailedWithCode(err, 51174);

// After building an index with the case-insensitive US English collation, the query should
// work. Furthermore, the bounds defined by the min and max should respect the
// case-insensitive collation.
assert.commandWorked(coll.createIndex({str: 1}, {name: "withCollation", collation: {locale: "en_US", strength: 2}}));
assert.eq(
    4,
    coll
        .find()
        .min({str: "b"})
        .max({str: "D"})
        .collation({locale: "en_US", strength: 2})
        .hint("withCollation")
        .itcount(),
);

// Ensure results from index with min/max query are sorted to match requested collation.
coll = testDb.collation_minmax2;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, b: 1}));
assert.commandWorked(
    coll.insert([
        {a: 1, b: 1},
        {a: 1, b: 2},
        {a: 1, b: "A"},
        {a: 1, b: "a"},
        {a: 2, b: 2},
    ]),
);
let expected = [
    {a: 1, b: 1},
    {a: 1, b: 2},
    {a: 1, b: "a"},
    {a: 1, b: "A"},
    {a: 2, b: 2},
];
res = coll
    .find({}, {_id: 0})
    .hint({a: 1, b: 1})
    .min({a: 1, b: 1})
    .max({a: 2, b: 3})
    .collation({locale: "en_US", strength: 3})
    .sort({a: 1, b: 1});
assert.eq(res.toArray(), expected);
res = coll
    .find({}, {_id: 0})
    .hint({a: 1, b: 1})
    .min({a: 1, b: 1})
    .collation({locale: "en_US", strength: 3})
    .sort({a: 1, b: 1});
assert.eq(res.toArray(), expected);
res = coll
    .find({}, {_id: 0})
    .hint({a: 1, b: 1})
    .max({a: 2, b: 3})
    .collation({locale: "en_US", strength: 3})
    .sort({a: 1, b: 1});
assert.eq(res.toArray(), expected);

// A min/max query that can use an index whose collation doesn't match should require a sort
// stage if there are any in-bounds strings. Verify this using explain.
explainRes = coll
    .find({}, {_id: 0})
    .hint({a: 1, b: 1})
    .max({a: 2, b: 3})
    .collation({locale: "en_US", strength: 3})
    .sort({a: 1, b: 1})
    .explain();
assert.commandWorked(explainRes);
assert(planHasStage(testDb, getWinningPlanFromExplain(explainRes.queryPlanner), "SORT"));

// This query should fail since min has a string as one of it's boundaries, and the
// collation doesn't match that of the index.
assert.throws(() =>
    coll
        .find({}, {_id: 0})
        .hint({a: 1, b: 1})
        .min({a: 1, b: "A"})
        .max({a: 2, b: 1})
        .collation({locale: "en_US", strength: 3})
        .sort({a: 1, b: 1})
        .itcount(),
);

// Ensure that attempting to create the same view namespace twice, but with different collations,
// fails with NamespaceExists.
res = testDb.runCommand({create: "view", viewOn: "coll"});
assert(res.ok == 1 || res.errmsg == ErrorCodes.NamespaceExists);
res = testDb.runCommand({create: "view", viewOn: "coll", collation: {locale: "en"}});
assert.commandFailedWithCode(res, ErrorCodes.NamespaceExists);
