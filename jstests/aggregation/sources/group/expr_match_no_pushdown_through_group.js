/**
 * Tests that a $match with $expr is NOT incorrectly pushed down through an upstream $group when
 * the $expr references a field name introduced by the $group (either the group key '_id' or one
 * of its accumulator outputs).
 *
 * On a buggy planner, the $match would be swapped before $group and applied to source documents.
 * Because the accumulator output (e.g. 'total') does not exist on the source documents, the
 * comparison '$gt: ["$total", 100]' would always evaluate to false (missing > 100 is false), and
 * the pipeline would incorrectly return zero rows.
 *
 * The semantics of $group can also differ from raw-document semantics for the '_id' field: null
 * and missing collapse into the same bucket, and numerically equal values of different numeric
 * types collapse into the same bucket. A pushed-down $expr against the source documents would
 * therefore observe distinctions that $group has already erased, dropping rows that should be
 * kept post-grouping.
 *
 * This test asserts that the pipeline result matches the semantics of "$group first, then $match"
 * regardless of any pushdown heuristics the planner applies.
 */
import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];

// ----------------------------------------------------------------------------------------------
// Case 1: $match{$expr} referencing an accumulator output ('total') introduced by $group.
//
// Without pushdown, '$gt: ["$total", 100]' is evaluated against the per-group result documents
// produced by $group, so groups whose summed amount exceeds 100 are retained.
//
// With incorrect pushdown, the $match is applied to source documents that have no 'total' field,
// so the predicate always evaluates to false and every row is dropped.
// ----------------------------------------------------------------------------------------------
coll.drop();
assert.commandWorked(
    coll.insert([
        {_id: 1, k: "a", amount: 60},
        {_id: 2, k: "a", amount: 70},   // group "a" sums to 130 -> kept
        {_id: 3, k: "b", amount: 40},
        {_id: 4, k: "b", amount: 50},   // group "b" sums to 90  -> dropped
        {_id: 5, k: "c", amount: 200},  // group "c" sums to 200 -> kept
        {_id: 6, k: "d", amount: 10},   // group "d" sums to 10  -> dropped
    ]),
);

// Compute the expected output by running the two stages independently of any pushdown decision.
const groupedOnly = coll.aggregate([{$group: {_id: "$k", total: {$sum: "$amount"}}}]).toArray();
const expectedCase1 = groupedOnly.filter((doc) => doc.total > 100);

const actualCase1 = coll
    .aggregate([
        {$group: {_id: "$k", total: {$sum: "$amount"}}},
        {$match: {$expr: {$gt: ["$total", 100]}}},
    ])
    .toArray();

assert(
    resultsEq(actualCase1, expectedCase1),
    `case 1 mismatch: actual=${tojson(actualCase1)} expected=${tojson(expectedCase1)}`,
);
// Sanity guard: a planner that pushes the $expr predicate down to source documents (where the
// 'total' field is missing) returns zero rows. Two non-empty groups should survive.
assert.eq(actualCase1.length, 2, `expected 2 groups post-$match, got ${tojson(actualCase1)}`);

// ----------------------------------------------------------------------------------------------
// Case 2: $match{$expr} referencing the group key '_id', where the $group has erased the
// null/missing distinction. Pushing the $expr predicate before $group would re-introduce the
// distinction against the source documents.
//
// This mirrors the canonical reproduction from the originating ticket: when 'a' is null and
// 'a' is missing, $group collapses them into a single _id:null bucket. A subsequent
// '$match: {$expr: {$eq: ["$_id", null]}}' should keep that bucket intact. With incorrect
// pushdown, the predicate runs against source documents where the missing-'a' document has
// '$_id' bound to the document's own _id (a non-null int), so that row is dropped before
// grouping and the post-group count is wrong.
// ----------------------------------------------------------------------------------------------
coll.drop();
assert.commandWorked(coll.insert([{_id: 1, a: null}, {_id: 2}]));

const expectedCase2 = [{_id: null, n: 2}];
const actualCase2 = coll
    .aggregate([
        {$group: {_id: "$a", n: {$count: {}}}},
        {$match: {$expr: {$eq: ["$_id", null]}}},
    ])
    .toArray();
assert(
    resultsEq(actualCase2, expectedCase2),
    `case 2 mismatch: actual=${tojson(actualCase2)} expected=${tojson(expectedCase2)}`,
);

// ----------------------------------------------------------------------------------------------
// Case 3: $match{$expr} with a $type predicate against the group key, where $group has erased
// the int-vs-long distinction (numerically equal numbers collapse). A $match keyed off the
// post-$group _id type should retain the merged bucket; pushdown that evaluates against the
// source documents would split the bucket and drop rows.
// ----------------------------------------------------------------------------------------------
coll.drop();
assert.commandWorked(
    coll.insert([
        {_id: 1, a: NumberInt(5)},
        {_id: 2, a: NumberLong(5)},
    ]),
);

const expectedCase3 = [{_id: 5, n: 2}];
const actualCase3 = coll
    .aggregate([
        {$group: {_id: "$a", n: {$count: {}}}},
        {$match: {$expr: {$eq: ["int", {$type: "$_id"}]}}},
    ])
    .toArray();
assert(
    resultsEq(actualCase3, expectedCase3),
    `case 3 mismatch: actual=${tojson(actualCase3)} expected=${tojson(expectedCase3)}`,
);

// ----------------------------------------------------------------------------------------------
// Case 4: Compound id with both a grouped key and an accumulator. Confirms the rule generalises
// beyond '_id' alone: the $match references both 'total' (an accumulator output) and '_id.k'
// (part of a compound group key). Neither should be pushed before $group.
// ----------------------------------------------------------------------------------------------
coll.drop();
assert.commandWorked(
    coll.insert([
        {_id: 1, k: "a", g: 1, amount: 80},
        {_id: 2, k: "a", g: 1, amount: 50},
        {_id: 3, k: "b", g: 1, amount: 200},
        {_id: 4, k: "a", g: 2, amount: 25},
    ]),
);

const groupedCompound = coll
    .aggregate([{$group: {_id: {k: "$k", g: "$g"}, total: {$sum: "$amount"}}}])
    .toArray();
const expectedCase4 = groupedCompound.filter((doc) => doc.total > 100 && doc._id.k === "a");

const actualCase4 = coll
    .aggregate([
        {$group: {_id: {k: "$k", g: "$g"}, total: {$sum: "$amount"}}},
        {
            $match: {
                $expr: {$and: [{$gt: ["$total", 100]}, {$eq: ["$_id.k", "a"]}]},
            },
        },
    ])
    .toArray();

assert(
    resultsEq(actualCase4, expectedCase4),
    `case 4 mismatch: actual=${tojson(actualCase4)} expected=${tojson(expectedCase4)}`,
);
assert.eq(actualCase4.length, 1, `expected exactly one compound group, got ${tojson(actualCase4)}`);

// ----------------------------------------------------------------------------------------------
// Case 5: $match{$expr} referencing ONLY a source-document field (one that is not produced or
// reshaped by $group) is a valid pushdown candidate, but must not change observable results.
// We assert correctness of the final output; whether the planner pushes the predicate or not
// is an internal optimisation that should be result-preserving.
// ----------------------------------------------------------------------------------------------
coll.drop();
assert.commandWorked(
    coll.insert([
        {_id: 1, k: "a", region: "us", amount: 10},
        {_id: 2, k: "a", region: "us", amount: 20},
        {_id: 3, k: "b", region: "eu", amount: 30},
        {_id: 4, k: "b", region: "us", amount: 40},
    ]),
);

// Field 'region' is preserved through $group only via $first; the $match below references
// 'region' which is a legitimate accumulator output of $group. The expected result is the
// post-group filter, which serves as the source of truth regardless of pushdown.
const groupedRegions = coll
    .aggregate([{$group: {_id: "$k", region: {$first: "$region"}, total: {$sum: "$amount"}}}])
    .toArray();
const expectedCase5 = groupedRegions.filter((doc) => doc.region === "us");

const actualCase5 = coll
    .aggregate([
        {$group: {_id: "$k", region: {$first: "$region"}, total: {$sum: "$amount"}}},
        {$match: {$expr: {$eq: ["$region", "us"]}}},
    ])
    .toArray();

assert(
    resultsEq(actualCase5, expectedCase5),
    `case 5 mismatch: actual=${tojson(actualCase5)} expected=${tojson(expectedCase5)}`,
);
