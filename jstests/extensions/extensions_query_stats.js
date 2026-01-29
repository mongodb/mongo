/**
 * Tests that extension query shapes can be collected and reparsed for $queryStats.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {kDefaultQueryStatsHmacKey, getQueryStatsWithTransform} from "jstests/libs/query/query_stats_utils.js";

// $queryStats is node-local, when multiple mongos are deployed in the cluster, each mongos will have its own
// separate query stats store. To avoid test failures due to this, we pin the test to a single mongos.
// pinToSingleMongos due to $queryStats command.
TestData.pinToSingleMongos = true;

// Use a unique db and coll for every test so burn_in_tests can run this test multiple times.
const collName = jsTestName() + Random.srand();
const foreignCollName = collName + "_foreign";
const testDb = db.getSiblingDB("extensions_query_stats_db" + Random.srand());
const coll = testDb[collName];
const foreignColl = testDb[foreignCollName];
coll.drop();
foreignColl.drop();

function insertLiteralShape(coll) {
    // Shape with literals. Test different values to ensure they all map to the same shape.
    let literalShape = {
        int: 123,
        double: 100.5,
        str: "abc",
        bool: false,
        nonEmptyObj: {a: 1, b: "two"},
        intArr: [1, 2, 3],
        mixedArr: [1, "hello"],

        // Make sure we don't choke on empty fields.
        emptyStr: "",
        zero: 0,
        emptyArr: [],
        emptyObj: {},
        undefinedField: undefined,
        nullField: null,
    };
    coll.aggregate([{$shapify: literalShape}]).toArray();

    literalShape.int = 100;
    coll.aggregate([{$shapify: literalShape}]).toArray();

    literalShape.str = "xyz";
    coll.aggregate([{$shapify: literalShape}]).toArray();

    literalShape.bool = true;
    coll.aggregate([{$shapify: literalShape}]).toArray();

    literalShape.nonEmptyObj = {c: 5};
    coll.aggregate([{$shapify: literalShape}]).toArray();

    literalShape.nonEmptyObj = {c: 5};
    coll.aggregate([{$shapify: literalShape}]).toArray();

    literalShape.intArr = [10];
    coll.aggregate([{$shapify: literalShape}]).toArray();

    literalShape.mixedArr = [5, true];
    coll.aggregate([{$shapify: literalShape}]).toArray();

    // Expected shape.
    return [
        {
            "$shapify": {
                "int": "?number",
                "double": "?number",
                "str": "?string",
                "bool": "?bool",
                "nonEmptyObj": "?object",
                "intArr": "?array<?number>",
                "mixedArr": "?array<>",
                "emptyStr": "?string",
                "zero": "?number",
                "emptyArr": "[]",
                "emptyObj": "?object",
                "undefinedField": "?undefined",
                "nullField": "?null",
            },
        },
    ];
}

function insertShapeWithSubobj(coll) {
    coll.aggregate([
        {
            $shapify: {
                obj_nestedOnce: {
                    int: 5,
                    str: "hello",
                    obj_nestedTwice: {
                        anotherInt: 10,
                        ident_name: "Alice",
                    },
                },
            },
        },
    ]).toArray();

    // Expected shape.
    return [
        {
            "$shapify": {
                "obj_nestedOnce": {
                    "int": "?number",
                    "str": "?string",
                    "obj_nestedTwice": {
                        "anotherInt": "?number",
                        "ident_name": "Alice",
                    },
                },
            },
        },
    ];
}

const identifiersShape = {
    ident_name: "Alice",
    ident_emptyName: "",
    // Empty field paths are not allowed, so we don't cover that here.
    path_fieldPathA: "a",
    path_fieldPathB: "b",
};

function fetchHmac(payload) {
    return computeSHA256Hmac({key: kDefaultQueryStatsHmacKey, payload});
}

var transformedIdentifiersShape = {};
for (const key in identifiersShape) {
    transformedIdentifiersShape[key] = fetchHmac(identifiersShape[key]);
}

function insertShapeWithIdentifiers1(coll) {
    const identifiersShape1 = Object.assign({}, identifiersShape);

    coll.aggregate([{$shapify: identifiersShape1}]).toArray();

    // This test does not transform identifiers, so the resulting shape is just the original identifier.
    return [{$shapify: identifiersShape1}];
}

function insertShapeWithIdentifiers2(coll) {
    const identifiersShape2 = Object.assign({}, identifiersShape, {ident_name: "Bob"});

    coll.aggregate([{$shapify: identifiersShape2}]).toArray();

    // This test does not transform identifiers, so the resulting shape is just the original identifier.
    return [{$shapify: identifiersShape2}];
}

function insertShapeWithIdentifiers3(coll) {
    const identifiersShape3 = Object.assign({}, identifiersShape, {path_fieldPathA: "a.b.c"});

    coll.aggregate([{$shapify: identifiersShape3}]).toArray();

    // This test does not transform identifiers, so the resulting shape is just the original identifier.
    return [{$shapify: identifiersShape3}];
}

function insertShapeWithTransformedIdentifiers4(coll) {
    const identifiersShape4 = Object.assign({}, identifiersShape, {ident_name: "Caroline"});

    coll.aggregate([{$shapify: identifiersShape4}]).toArray();

    const expected = Object.assign({}, transformedIdentifiersShape, {ident_name: fetchHmac("Caroline")});
    return [{$shapify: expected}];
}

function insertShapeWithTransformedIdentifiers5(coll) {
    const identifiersShape5 = Object.assign({}, identifiersShape, {path_fieldPathA: "d.e.f"});

    coll.aggregate([{$shapify: identifiersShape5}]).toArray();

    const hmacedFieldPath = fetchHmac("d") + "." + fetchHmac("e") + "." + fetchHmac("f");
    const expected = Object.assign({}, transformedIdentifiersShape, {path_fieldPathA: hmacedFieldPath});
    return [{$shapify: expected}];
}

function insertShapeWithTransformedIdentifiers6(coll) {
    const identifiersShape5 = Object.assign({}, identifiersShape, {path_fieldPathA: "g.h.1"});

    coll.aggregate([{$shapify: identifiersShape5}]).toArray();

    const hmacedFieldPath = fetchHmac("g") + "." + fetchHmac("h") + "." + fetchHmac("1");
    const expected = Object.assign({}, transformedIdentifiersShape, {path_fieldPathA: hmacedFieldPath});
    return [{$shapify: expected}];
}

/**
 * Fetch query stats entries for the collection, sorted by most recently executed first.
 */
function fetchQueryStats(db, transformIdentifiers) {
    return getQueryStatsWithTransform(
        db,
        {},
        {
            collName: transformIdentifiers ? fetchHmac(collName) : collName,
            transformIdentifiers,
            customSort: {"metrics.latestSeenTimestamp": -1},
        },
    );
}

// Track the number of query stats entries to ensure each test adds exactly one new shape.
let statsSoFar = 0;

/**
 * Runs a query stats test: executes aggregation(s) and validates the resulting query shape.
 *
 * Supports two modes:
 *   1. Direct: provide `pipeline` and `expectedShape` for simple single-aggregation tests.
 *   2. Callback: provide `runFn` for complex tests that run multiple aggregations
 *      (e.g., testing that different literal values map to the same shape).
 *
 * @param {Object} opts - Test options.
 * @param {string} opts.desc - Test description for logging.
 * @param {Array} [opts.pipeline] - (Direct) The aggregation pipeline to run.
 * @param {Array} [opts.expectedShape] - (Direct) The expected query shape.
 * @param {Function} [opts.runFn] - (Callback) Function that runs aggregation(s) and returns expected shape.
 * @param {boolean} [opts.transformIdentifiers=false] - Whether to transform identifiers in $queryStats.
 */
function runQueryStatsTest({desc, pipeline, expectedShape, runFn, transformIdentifiers = false}) {
    jsTest.log.info("Testing $queryStats for " + desc);

    let expected;
    if (runFn) {
        // Callback mode: runFn executes aggregation(s) and returns expected shape.
        expected = runFn(coll);
    } else {
        // Direct mode: run the provided pipeline.
        coll.aggregate(pipeline).toArray();
        expected = expectedShape;
    }

    const stats = fetchQueryStats(testDb, transformIdentifiers);

    // Verify that exactly one new shape was added. This catches cases where multiple queries
    // that should map to the same shape accidentally create distinct shapes.
    assert.eq(statsSoFar + 1, stats.length, stats);
    statsSoFar = stats.length;

    const latestShape = stats[0].key.queryShape.pipeline;
    assert.docEq(expected, latestShape, stats);

    // We purposefully sleep for a very short amount of time here. It is possible for the
    // tests to run so fast that two of the queries have the same `lastSeenTimestamp`, which
    // messes up the query stats sorting since the minimum resolution is 1ms.
    sleep(100);
}

// Enable query stats collection.
testDb.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1});

// =============================================================================
// Basic extension stage shape tests
// =============================================================================
runQueryStatsTest({
    desc: "empty shape",
    pipeline: [{$shapify: {}}],
    expectedShape: [{$shapify: {}}],
});

// Test that a desugar stage's query shape is calculated from the pre-desugared stage.
runQueryStatsTest({
    desc: "desugar stage shape",
    pipeline: [{$shapifyDesugar: {a: 1, b: 2}}],
    expectedShape: [{$shapifyDesugar: {a: 1, b: 2}}],
});

// Test that different literal values all map to the same shape.
runQueryStatsTest({
    desc: "literal shape with multiple variations",
    runFn: insertLiteralShape,
});

runQueryStatsTest({
    desc: "shape with nested objects",
    runFn: insertShapeWithSubobj,
});

// =============================================================================
// Identifier shape tests (without transformation)
// =============================================================================
runQueryStatsTest({
    desc: "shape with identifiers (Alice)",
    runFn: insertShapeWithIdentifiers1,
});

runQueryStatsTest({
    desc: "shape with identifiers (Bob)",
    runFn: insertShapeWithIdentifiers2,
});

runQueryStatsTest({
    desc: "shape with identifiers (nested field path)",
    runFn: insertShapeWithIdentifiers3,
});

// =============================================================================
// Identifier shape tests (with HMAC transformation)
// =============================================================================
runQueryStatsTest({
    desc: "transformed identifiers (Caroline)",
    runFn: insertShapeWithTransformedIdentifiers4,
    transformIdentifiers: true,
});

runQueryStatsTest({
    desc: "transformed identifiers (d.e.f field path)",
    runFn: insertShapeWithTransformedIdentifiers5,
    transformIdentifiers: true,
});

runQueryStatsTest({
    desc: "transformed identifiers (g.h.1 field path)",
    runFn: insertShapeWithTransformedIdentifiers6,
    transformIdentifiers: true,
});

// =============================================================================
// $unionWith with extension stages
// =============================================================================
runQueryStatsTest({
    desc: "extension stage in $unionWith subpipeline",
    pipeline: [{$unionWith: {coll: foreignCollName, pipeline: [{$shapify: {a: 1}}]}}],
    expectedShape: [{$unionWith: {coll: foreignCollName, pipeline: [{$shapify: {a: "?number"}}]}}],
});

// Test desugar extension stage in $unionWith subpipeline.
// The query shape should preserve the pre-desugared $shapifyDesugar stage.
// $shapifyDesugar does not anonymize its arguments.
runQueryStatsTest({
    desc: "desugar extension stage in $unionWith subpipeline",
    pipeline: [{$unionWith: {coll: foreignCollName, pipeline: [{$shapifyDesugar: {a: 1, b: 2}}]}}],
    expectedShape: [{$unionWith: {coll: foreignCollName, pipeline: [{$shapifyDesugar: {a: 1, b: 2}}]}}],
});

// Test extension stage with regular stages in $unionWith subpipeline.
runQueryStatsTest({
    desc: "mixed pipeline with extension stage in $unionWith",
    pipeline: [
        {$match: {x: 1}},
        {$unionWith: {coll: foreignCollName, pipeline: [{$shapify: {val: 100}}, {$limit: 5}]}},
    ],
    expectedShape: [
        {$match: {x: {$eq: "?number"}}},
        {$unionWith: {coll: foreignCollName, pipeline: [{$shapify: {val: "?number"}}, {$limit: "?number"}]}},
    ],
});

// Test nested $unionWith with extension stages.
runQueryStatsTest({
    desc: "nested $unionWith with extension stages",
    pipeline: [
        {
            $unionWith: {
                coll: foreignCollName,
                pipeline: [
                    {$shapify: {outer: true}},
                    {$unionWith: {coll: collName, pipeline: [{$shapify: {inner: false}}]}},
                ],
            },
        },
    ],
    expectedShape: [
        {
            $unionWith: {
                coll: foreignCollName,
                pipeline: [
                    {$shapify: {outer: "?bool"}},
                    {$unionWith: {coll: collName, pipeline: [{$shapify: {inner: "?bool"}}]}},
                ],
            },
        },
    ],
});
