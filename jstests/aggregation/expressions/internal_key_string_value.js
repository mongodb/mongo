/**
 * Tests the behavior of $_internalKeyStringValue when used in agg expressions.
 *
 * @tags: [
 *   assumes_stable_shard_list,
 *   multiversion_incompatible,
 *   not_allowed_with_signed_security_token,
 * ]
 */

import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const coll = db[jsTestName()];
assert.commandWorked(db.createCollection(coll.getName()));

// Clears the collection without dropping it, avoiding a sharded drop/recreate race.
function clearCollection() {
    assert.commandWorked(db.runCommand({delete: coll.getName(), deletes: [{q: {}, limit: 0}]}));
}

// Testing behavior for basic data types.
clearCollection();
let docs = [
    {_id: 0, a: NumberInt(1)},
    {_id: 1, a: NumberLong(2)},
    {_id: 2, a: 3.0},
    {_id: 3, a: NumberDecimal("4.000000")},
    {_id: 4, a: "abc"},
    {_id: 5, a: ISODate("2024-01-01")},
    {_id: 6, a: [1, 2, 3]},
    {_id: 7, a: {b: 1, c: 2, d: 3}},
];
assert.commandWorked(coll.insert(docs));

let results = coll
    .aggregate([{$sort: {_id: 1}}, {$addFields: {b: {$_internalKeyStringValue: {input: "$a"}}}}])
    .toArray();
assert.eq(
    results,
    [
        {_id: 0, a: NumberInt(1), b: BinData(0, "KwIE")},
        {_id: 1, a: NumberLong(2), b: BinData(0, "KwQE")},
        {_id: 2, a: 3.0, b: BinData(0, "KwYE")},
        {_id: 3, a: NumberDecimal("4.000000"), b: BinData(0, "KwgE")},
        {_id: 4, a: "abc", b: BinData(0, "PGFiYwAE")},
        {_id: 5, a: ISODate("2024-01-01"), b: BinData(0, "eIAAAYzCUfQABA==")},
        {_id: 6, a: [1, 2, 3], b: BinData(0, "UCsCKwQrBgAE")},
        {_id: 7, a: {b: 1, c: 2, d: 3}, b: BinData(0, "Rh5iACsCHmMAKwQeZAArBgAE")},
    ],
    results,
);

// Testing behavior for same numeric values of different types.
clearCollection();
docs = [
    {_id: 0, a: NumberInt(1)},
    {_id: 1, a: NumberLong(1)},
    {_id: 2, a: 1.0},
    {_id: 3, a: NumberDecimal("1.000")},
    {_id: 4, a: NumberDecimal("1.000000")},
];
assert.commandWorked(coll.insert(docs));

results = coll
    .aggregate([{$sort: {_id: 1}}, {$addFields: {b: {$_internalKeyStringValue: {input: "$a"}}}}])
    .toArray();
assert.eq(docs.length, results.length, results);
assert(
    results.every((result) => bsonWoCompare(result.b, results[0].b) === 0),
    results,
);

// Testing behavior for same numeric values of different types inside object.
clearCollection();
docs = [
    {_id: 0, a: {b: NumberInt(1)}},
    {_id: 1, a: {b: 1.0}},
    {_id: 2, a: {b: NumberDecimal("1.000")}},
    {_id: 3, a: {b: NumberDecimal("1.000000")}},
];
assert.commandWorked(coll.insert(docs));

results = coll
    .aggregate([{$sort: {_id: 1}}, {$addFields: {c: {$_internalKeyStringValue: {input: "$a"}}}}])
    .toArray();
assert.eq(docs.length, results.length, results);
assert(
    results.every((result) => bsonWoCompare(result.c, results[0].c) === 0),
    results,
);

// Testing behavior for close numeric values.
clearCollection();
docs = [
    {_id: 0, a: 1},
    {_id: 1, a: 1.00001},
];
assert.commandWorked(coll.insert(docs));

results = coll
    .aggregate([{$sort: {_id: 1}}, {$addFields: {b: {$_internalKeyStringValue: {input: "$a"}}}}])
    .toArray();
assert.eq(2, results.length, results);
assert(bsonWoCompare(results[0].b, results[1].b) !== 0, results);

// Testing behavior for large numeric values.
clearCollection();
docs = [
    {_id: 0, a: 1e20},
    {_id: 1, a: NumberDecimal("1e21")},
];
assert.commandWorked(coll.insert(docs));

results = coll
    .aggregate([
        {$sort: {_id: 1}},
        {
            $addFields: {
                b: {$_internalKeyStringValue: {input: "$a"}},
                c: {$toHashedIndexKey: "$a"},
            },
        },
    ])
    .toArray();
assert.eq(2, results.length, results);
assert(bsonWoCompare(results[0].b, results[1].b) !== 0, results);
// $toHashedIndexKey hashes large numbers greater than 2^63 to the same result.
assert(bsonWoCompare(results[0].c, results[1].c) === 0, results);

// Testing behavior for strings under case-sensitive collation that doesn't match.
clearCollection();
docs = [
    {_id: 0, a: "aAa"},
    {_id: 1, a: "AaA"},
];
assert.commandWorked(coll.insert(docs));

results = coll
    .aggregate([
        {$sort: {_id: 1}},
        {
            $addFields: {
                b: {
                    $_internalKeyStringValue: {input: "$a", collation: {locale: "en", strength: 3}},
                },
            },
        },
    ])
    .toArray();
assert.eq(2, results.length, results);
assert(bsonWoCompare(results[0].b, results[1].b) !== 0, results);

// Testing behavior for strings under case-sensitive collation that matches.
clearCollection();
docs = [
    {_id: 0, a: "aAa"},
    {_id: 1, a: "aAa"},
];
assert.commandWorked(coll.insert(docs));

results = coll
    .aggregate([
        {$sort: {_id: 1}},
        {
            $addFields: {
                b: {
                    $_internalKeyStringValue: {input: "$a", collation: {locale: "en", strength: 3}},
                },
            },
        },
    ])
    .toArray();
assert.eq(docs.length, results.length, results);
assert(
    results.every((result) => bsonWoCompare(result.c, results[0].c) === 0),
    results,
);

// Testing behavior for strings under case-insensitive collation that doesn't match.
clearCollection();
docs = [
    {_id: 0, a: "aAa"},
    {_id: 1, a: "aBa"},
];
assert.commandWorked(coll.insert(docs));

results = coll
    .aggregate([
        {$sort: {_id: 1}},
        {
            $addFields: {
                b: {
                    $_internalKeyStringValue: {input: "$a", collation: {locale: "en", strength: 1}},
                },
            },
        },
    ])
    .toArray();
assert.eq(2, results.length, results);
assert(bsonWoCompare(results[0].b, results[1].b) !== 0, results);

// Testing behavior for strings under case-insensitive collation that matches.
clearCollection();
docs = [
    {_id: 0, a: "aAa"},
    {_id: 1, a: "AaA"},
];
assert.commandWorked(coll.insert(docs));

results = coll
    .aggregate([
        {$sort: {_id: 1}},
        {
            $addFields: {
                b: {
                    $_internalKeyStringValue: {input: "$a", collation: {locale: "en", strength: 1}},
                },
            },
        },
    ])
    .toArray();
assert.eq(docs.length, results.length, results);
assert(
    results.every((result) => bsonWoCompare(result.b, results[0].b) === 0),
    results,
);

// Lower the per-query memory limit ('internalQueryMaxMemoryUsageBytesPerOperation') to check that a
// large input trips it whether or not a collation is requested.
const bigArray = Array.from({length: 500}, (_, i) => "x".repeat(4096) + i);

clearCollection();
assert.commandWorked(coll.insert({_id: 0}));

function runValue(input, collation) {
    const spec = {input};
    if (collation !== undefined) {
        spec.collation = collation;
    }
    return db.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$project: {k: {$_internalKeyStringValue: spec}}}],
        cursor: {},
    });
}

// Under the default limit, a multi-megabyte input succeeds.
assert.commandWorked(runValue(bigArray, {locale: "en", strength: 1}));
assert.commandWorked(runValue(bigArray));

const kRestrictedLimit = 1024 * 1024;
runWithParamsAllNonConfigNodes(
    db,
    {internalQueryMaxMemoryUsageBytesPerOperation: kRestrictedLimit},
    () => {
        assert.commandFailedWithCode(
            runValue(bigArray),
            ErrorCodes.ExceededMemoryLimit,
            "a large input's copied BSON must hit the per-query memory limit without a collation",
        );

        assert.commandFailedWithCode(
            runValue(bigArray, {locale: "en", strength: 1}),
            ErrorCodes.ExceededMemoryLimit,
            "a large collation-aware input must hit the per-query memory limit",
        );
    },
);

assert.commandWorked(runValue(["a", "b"], {locale: "en", strength: 1}));
