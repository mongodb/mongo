/**
 * Tests that SBE can use a single-path WILDCARD index on the foreign side of a $lookup and still
 * return correct results. Like a sparse index, a wildcard index omits documents missing the
 * indexed field, so SBE uses the dynamic indexed loop join (DILJ), falling back to a collection
 * scan for null/missing/array/object values.
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {getAggPlanStages, getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";

const conn = MongoRunner.runMongod({setParameter: {internalQueryFrameworkControl: "trySbeEngine"}});
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB(jsTestName());

if (!checkSbeRestrictedOrFullyEnabled(db)) {
    jsTest.log.info("Skipping test because SBE is disabled");
    MongoRunner.stopMongod(conn);
    quit();
}

const local = db.local;
const foreign = db.foreign;

// Local docs: a scalar, a missing field, an explicit null, an array, and an object.
const localDocs = [
    {_id: 1, a: "apple"},
    {_id: 2},
    {_id: 3, a: null},
    {_id: 4, a: [1, 2]},
    {_id: 5, a: {x: 1}},
];
// Foreign docs: one matching value, one explicit null, one missing "a" entirely, one array, one
// object.
const foreignDocs = [
    {_id: 10, a: "apple", details: "red fruit"},
    {_id: 11, a: null, details: "unknown value"},
    {_id: 12, details: "missing 'a' field entirely"},
    {_id: 13, a: [1, 2], details: "array value"},
    {_id: 14, a: {x: 1}, details: "object value"},
];

const expected = [
    {_id: 1, matchedIds: [10]},
    {_id: 2, matchedIds: [11, 12]},
    {_id: 3, matchedIds: [11, 12]},
    {_id: 4, matchedIds: [13]},
    {_id: 5, matchedIds: [14]},
];

const pipeline = [
    {$lookup: {from: foreign.getName(), localField: "a", foreignField: "a", as: "m"}},
    {$project: {matchedIds: {$sortArray: {input: "$m._id", sortBy: 1}}}},
];

function reset(foreignIndexSpecs) {
    local.drop();
    foreign.drop();
    assert.commandWorked(local.insert(localDocs));
    assert.commandWorked(foreign.insert(foreignDocs));
    for (const spec of foreignIndexSpecs) {
        assert.commandWorked(foreign.createIndex(spec.key, spec.options));
    }
}

// Runs the pipeline and asserts the chosen SBE strategy and correct results.
function runCase({label, indexes, aggOptions, expectedStrategy}) {
    reset(indexes);

    const planExplain = local.explain().aggregate(pipeline, aggOptions);
    assert.eq(
        "sbe",
        getEngine(planExplain),
        `${label}: expected SBE pushdown: ${tojson(planExplain)}`,
    );
    const eqLookupNodes = getAggPlanStages(planExplain, "EQ_LOOKUP");
    assert.eq(1, eqLookupNodes.length, `${label}: expected one EQ_LOOKUP: ${tojson(planExplain)}`);
    assert.eq(
        expectedStrategy,
        eqLookupNodes[0].strategy,
        `${label}: unexpected strategy: ${tojson(planExplain)}`,
    );

    const results = local.aggregate(pipeline, aggOptions).toArray();
    assertArrayEq({
        actual: results,
        expected,
        extraErrorMsg: `${label}: incorrect $lookup results`,
    });
}

// Case 1: single-path wildcard index, HashJoin disabled (allowDiskUse:false) -> DILJ.
runCase({
    label: "wildcard, DILJ",
    indexes: [{key: {"$**": 1}, options: {}}],
    aggOptions: {allowDiskUse: false},
    expectedStrategy: "DynamicIndexedLoopJoin",
});

// Case 2: wildcard index with HashJoin allowed -> HashJoin (also correct; it scans the foreign
// side).
runCase({
    label: "wildcard, HashJoin",
    indexes: [{key: {"$**": 1}, options: {}}],
    aggOptions: {allowDiskUse: true},
    expectedStrategy: "HashJoin",
});

// Case 3: when a non-sparse, non-wildcard index is also available, it is preferred and the faster
// INLJ is kept over the wildcard-backed DILJ.
runCase({
    label: "non-wildcard preferred",
    indexes: [{key: {a: 1}, options: {}}],
    aggOptions: {allowDiskUse: false},
    expectedStrategy: "IndexedLoopJoin",
});

// Case 4: a scoped wildcard index (on a different top-level field) does not cover the foreign
// field at all, so it is not eligible; falls back to HashJoin (no index).
runCase({
    label: "scoped wildcard on unrelated field falls back to HashJoin",
    indexes: [{key: {"other.$**": 1}, options: {}}],
    aggOptions: {allowDiskUse: true},
    expectedStrategy: "HashJoin",
});

// Case 4b: a PARTIAL wildcard index covering the foreign field must stay classic-only, since
// DILJ's runtime guards don't account for documents excluded by the filter.
{
    reset([{key: {"$**": 1}, options: {partialFilterExpression: {a: {$exists: true}}}}]);
    const planExplain = local.explain().aggregate(pipeline, {allowDiskUse: false});
    // Don't use getEngine() here: it reports "sbe" as soon as any part of the pipeline (e.g. the
    // initial collection scan) uses SBE, regardless of whether $lookup itself was pushed down.
    // Checking for the absence of a lowered EQ_LOOKUP node is the reliable signal.
    const eqLookupNodes = getAggPlanStages(planExplain, "EQ_LOOKUP");
    assert.eq(
        0,
        eqLookupNodes.length,
        `partial wildcard: expected no lowered EQ_LOOKUP stage: ${tojson(planExplain)}`,
    );
    const results = local.aggregate(pipeline, {allowDiskUse: false}).toArray();
    assertArrayEq({
        actual: results,
        expected,
        extraErrorMsg: `partial wildcard: incorrect $lookup results`,
    });
}

// Case 4c: a COMPOUND wildcard index covering the foreign field must also stay classic-only. SBE
// only supports single-path wildcard indexes for $lookup pushdown.
{
    reset([{key: {other: 1, "$**": 1}, options: {wildcardProjection: {a: 1}}}]);
    const planExplain = local.explain().aggregate(pipeline, {allowDiskUse: false});
    const eqLookupNodes = getAggPlanStages(planExplain, "EQ_LOOKUP");
    assert.eq(
        0,
        eqLookupNodes.length,
        `compound wildcard: expected no lowered EQ_LOOKUP stage: ${tojson(planExplain)}`,
    );
    const results = local.aggregate(pipeline, {allowDiskUse: false}).toArray();
    assertArrayEq({
        actual: results,
        expected,
        extraErrorMsg: `compound wildcard: incorrect $lookup results`,
    });
}

// Case 5: a foreign document holding the same value repeated within its own array must be
// deduped, not returned once per repetition.
{
    reset([{key: {"$**": 1}, options: {}}]);
    assert.commandWorked(foreign.insert({_id: 20, a: [9, 9, 9]}));
    const aggOptions = {allowDiskUse: false}; // force DILJ
    const planExplain = local.explain().aggregate(pipeline, aggOptions);
    assert.eq(
        "DynamicIndexedLoopJoin",
        getAggPlanStages(planExplain, "EQ_LOOKUP")[0].strategy,
        `repeated-in-own-array: ${tojson(planExplain)}`,
    );
    const localOne = db.local_one;
    localOne.drop();
    assert.commandWorked(localOne.insert({_id: 1, a: 9}));
    const results = localOne.aggregate(pipeline, aggOptions).toArray();
    assert.eq(1, results.length, tojson(results));
    assert.eq(
        [20],
        results[0].matchedIds,
        "expected exactly one match, not one per repeated array element: " + tojson(results[0]),
    );
}

// Case 6: two different local keys that both independently match the same foreign document (via
// its array) must not each contribute a separate copy of that document to the result.
{
    reset([{key: {"$**": 1}, options: {}}]);
    const localTwoKeys = db.local_two_keys;
    localTwoKeys.drop();
    assert.commandWorked(localTwoKeys.insert({_id: 1, a: [1, 2]}));
    const aggOptions = {allowDiskUse: false}; // force DILJ
    const results = localTwoKeys.aggregate(pipeline, aggOptions).toArray();
    assert.eq(1, results.length, tojson(results));
    assert.eq(
        [13],
        results[0].matchedIds,
        "expected exactly one match across multiple matching local keys: " + tojson(results[0]),
    );
}

// Case 7: a wildcard index whose collation is ALSO incompatible with the query. Both runtime
// guards must apply together via DILJ: collation-sensitive (string) and array/object local keys,
// as well as null/missing local keys, fall back to the collection scan, while collation-independent
// non-null scalar keys (e.g. numbers) still seek the index.
{
    const lc = db.combo_local;
    const fc = db.combo_foreign;
    lc.drop();
    fc.drop();
    assert.commandWorked(
        lc.insert([
            {_id: 1, a: "apple"}, // string -> collation guard -> scan
            {_id: 2, a: 7}, // number -> collation-independent and non-null -> index seek
            {_id: 3}, // missing -> wildcard guard -> scan
            {_id: 4, a: [1, 2]}, // array -> wildcard guard -> scan
        ]),
    );
    assert.commandWorked(
        fc.insert([
            {_id: 10, a: "Apple"}, // differs from local "apple" only by case
            {_id: 11, a: "apple"},
            {_id: 12, a: 7},
            {_id: 13, a: [1, 2]},
            {_id: 14}, // missing 'a' -> absent from the wildcard index
        ]),
    );
    // Wildcard index with a case-insensitive collation: incompatible with the (simple) query
    // collation, so DILJ must apply BOTH the collation type guard and the wildcard guard.
    assert.commandWorked(fc.createIndex({"$**": 1}, {collation: {locale: "en_US", strength: 2}}));

    const comboPipeline = [
        {$lookup: {from: fc.getName(), localField: "a", foreignField: "a", as: "m"}},
        {$project: {matchedIds: {$sortArray: {input: "$m._id", sortBy: 1}}}},
    ];
    const aggOptions = {allowDiskUse: false}; // force DILJ

    const planExplain = lc.explain().aggregate(comboPipeline, aggOptions);
    assert.eq("sbe", getEngine(planExplain), `combo: expected SBE: ${tojson(planExplain)}`);
    const nodes = getAggPlanStages(planExplain, "EQ_LOOKUP");
    assert.eq(1, nodes.length, `combo: expected one EQ_LOOKUP: ${tojson(planExplain)}`);
    assert.eq("DynamicIndexedLoopJoin", nodes[0].strategy, `combo: ${tojson(planExplain)}`);

    // Expected under the simple (case-sensitive) query collation:
    //   _id:1 "apple" -> only the exact lowercase "apple" (_id:11), NOT "Apple": proves the
    //                    case-insensitive index was NOT used for this string key (collation
    //                    guard).
    //   _id:2 7       -> _id:12, found via the index seek.
    //   _id:3 missing -> the missing-field doc (_id:14, absent from the wildcard index): a
    //                    missing local key is null-like and only matches null/missing foreign
    //                    values, proving the wildcard guard forced a scan for this key.
    //   _id:4 [1, 2]  -> the array doc (_id:13), via array-element intersection, proving the
    //                    wildcard guard forced a scan for this key too.
    const comboExpected = [
        {_id: 1, matchedIds: [11]},
        {_id: 2, matchedIds: [12]},
        {_id: 3, matchedIds: [14]},
        {_id: 4, matchedIds: [13]},
    ];
    const comboResults = lc.aggregate(comboPipeline, aggOptions).toArray();
    assertArrayEq({
        actual: comboResults,
        expected: comboExpected,
        extraErrorMsg: "combo (wildcard + incompatible collation) incorrect results",
    });
}

// Case 8: a wildcard index whose collation matches the query's, so collation-sensitive keys are
// safe to seek rather than scan. Regression test: the seek key's '$_path' marker was previously
// getting collated along with the value, so the seek always found nothing.
{
    const lc = db.matching_collation_local;
    const fc = db.matching_collation_foreign;
    const collation = {locale: "en_US"};
    lc.drop();
    fc.drop();
    assert.commandWorked(db.createCollection(lc.getName(), {collation}));
    assert.commandWorked(db.createCollection(fc.getName(), {collation}));
    assert.commandWorked(
        lc.insert([
            {_id: 1, a: "apple"}, // string -> collation-compatible index seek
            {_id: 2, a: 7}, // number -> index seek
            {_id: 3}, // missing -> wildcard guard -> scan
            {_id: 4, a: null}, // null -> wildcard guard -> scan
            {_id: 5, a: [1, 2]}, // array -> wildcard guard -> scan
            {_id: 6, a: "BANANA"}, // string differing only by case from a foreign value
        ]),
    );
    assert.commandWorked(
        fc.insert([
            {_id: 10, a: "apple"},
            {_id: 11, a: 7},
            {_id: 12}, // missing 'a' -> absent from the wildcard index
            {_id: 13, a: [1, 2]},
            {_id: 14, a: "banana"},
        ]),
    );
    // No explicit collation option: the index inherits the collection's default, matching the
    // query's collation.
    assert.commandWorked(fc.createIndex({"$**": 1}));

    const matchingCollationPipeline = [
        {$lookup: {from: fc.getName(), localField: "a", foreignField: "a", as: "m"}},
        {$project: {matchedIds: {$sortArray: {input: "$m._id", sortBy: 1}}}},
    ];
    const aggOptions = {allowDiskUse: false}; // force DILJ

    const planExplain = lc.explain().aggregate(matchingCollationPipeline, aggOptions);
    assert.eq(
        "sbe",
        getEngine(planExplain),
        `matching collation: expected SBE: ${tojson(planExplain)}`,
    );
    const nodes = getAggPlanStages(planExplain, "EQ_LOOKUP");
    assert.eq(
        1,
        nodes.length,
        `matching collation: expected one EQ_LOOKUP: ${tojson(planExplain)}`,
    );
    assert.eq(
        "DynamicIndexedLoopJoin",
        nodes[0].strategy,
        `matching collation: ${tojson(planExplain)}`,
    );

    // en_US at the default strength is case-sensitive, so "BANANA" does NOT match "banana" here;
    // that's real collation behavior, not a bug (verified separately against a plain find()).
    const matchingCollationExpected = [
        {_id: 1, matchedIds: [10]},
        {_id: 2, matchedIds: [11]},
        {_id: 3, matchedIds: [12]},
        {_id: 4, matchedIds: [12]},
        {_id: 5, matchedIds: [13]},
        {_id: 6, matchedIds: []},
    ];
    const matchingCollationResults = lc.aggregate(matchingCollationPipeline, aggOptions).toArray();
    assertArrayEq({
        actual: matchingCollationResults,
        expected: matchingCollationExpected,
        extraErrorMsg: "matching collation: incorrect $lookup results",
    });
}

// Case 8b: same as Case 8, but case-insensitive, proving the seek path itself (not just the scan
// fallback) can find a match that differs by case.
{
    const lc = db.matching_ci_collation_local;
    const fc = db.matching_ci_collation_foreign;
    const collation = {locale: "en_US", strength: 2};
    lc.drop();
    fc.drop();
    assert.commandWorked(db.createCollection(lc.getName(), {collation}));
    assert.commandWorked(db.createCollection(fc.getName(), {collation}));
    assert.commandWorked(lc.insert([{_id: 1, a: "BANANA"}]));
    assert.commandWorked(fc.insert([{_id: 10, a: "banana"}]));
    assert.commandWorked(fc.createIndex({"$**": 1}));

    const ciPipeline = [
        {$lookup: {from: fc.getName(), localField: "a", foreignField: "a", as: "m"}},
        {$project: {matchedIds: {$sortArray: {input: "$m._id", sortBy: 1}}}},
    ];
    const aggOptions = {allowDiskUse: false}; // force DILJ

    const planExplain = lc.explain().aggregate(ciPipeline, aggOptions);
    assert.eq(
        "DynamicIndexedLoopJoin",
        getAggPlanStages(planExplain, "EQ_LOOKUP")[0].strategy,
        `case-insensitive matching collation: ${tojson(planExplain)}`,
    );
    const results = lc.aggregate(ciPipeline, aggOptions).toArray();
    assert.eq(1, results.length, tojson(results));
    assert.eq(
        [10],
        results[0].matchedIds,
        "expected case-insensitive match via index seek: " + tojson(results[0]),
    );
}

MongoRunner.stopMongod(conn);
