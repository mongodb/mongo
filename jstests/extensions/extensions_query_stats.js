/**
 * Tests that extension query shapes can be collected and reparsed for $queryStats.
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {kDefaultQueryStatsHmacKey, getQueryStatsWithTransform} from "jstests/libs/query/query_stats_utils.js";

// Use a unique db and coll for every test so burn_in_tests can run this test multiple times.
const collName = jsTestName() + Random.srand();
const testDb = db.getSiblingDB("extensions_query_stats_db" + Random.srand());
const coll = testDb[collName];
coll.drop();

function insertEmptyShape(coll) {
    coll.aggregate([{$shapify: {}}]).toArray();

    // Expected shape.
    return [{"$shapify": {}}];
}

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

/**
 * Helper that runs the provided function to insert a single query shape and validates
 * the results returned by $queryStats. Returns an updated count of query stats entries
 * for the collection.
 */
function insertShapeAndValidateResults(db, insertShapeFn, statsSoFar, transformIdentifiers = false) {
    const coll = db[collName];

    const expectedShape = insertShapeFn(coll);
    const stats = fetchQueryStats(db, transformIdentifiers);

    assert.eq(statsSoFar + 1, stats.length, stats);
    statsSoFar = stats.length;

    const actualShape = stats[0].key.queryShape.pipeline;
    assert.docEq(expectedShape, actualShape, stats);

    // We purposefully sleep for a very short amount of time here. It is possible for the
    // tests to run so fast that two of the queries have the same `lastSeenTimestamp`, which
    // messes up the query stats sorting since the minimum resolution is 1ms.
    sleep(100);

    return stats.length;
}

// Enable query stats collection.
testDb.adminCommand({setParameter: 1, internalQueryStatsRateLimit: -1});

let statsSoFar = fetchQueryStats(testDb).length;

statsSoFar = insertShapeAndValidateResults(testDb, insertEmptyShape, statsSoFar);
statsSoFar = insertShapeAndValidateResults(testDb, insertLiteralShape, statsSoFar);
statsSoFar = insertShapeAndValidateResults(testDb, insertShapeWithSubobj, statsSoFar);

statsSoFar = insertShapeAndValidateResults(testDb, insertShapeWithIdentifiers1, statsSoFar);
statsSoFar = insertShapeAndValidateResults(testDb, insertShapeWithIdentifiers2, statsSoFar);
statsSoFar = insertShapeAndValidateResults(testDb, insertShapeWithIdentifiers3, statsSoFar);

// Transformed identifiers/field paths tests.
statsSoFar = insertShapeAndValidateResults(testDb, insertShapeWithTransformedIdentifiers4, statsSoFar, true);
statsSoFar = insertShapeAndValidateResults(testDb, insertShapeWithTransformedIdentifiers5, statsSoFar, true);
statsSoFar = insertShapeAndValidateResults(testDb, insertShapeWithTransformedIdentifiers6, statsSoFar, true);
