/**
 * This test was generated via an LLM that was prompted to find code paths that are special-cased for the _id field.
 *
 * Exercises the `_id` field across server code paths that treat it specially:
 *
 * - ID hack / express fast path: simple equality, `{_id: {$eq}}`, exact object match when the
 *   embedded object's first key is not an operator; planner exclusions (range, regex, hint,
 *   skip).
 * - `isSimpleIdQuery`-style filters used for upserts (including `{_id: {$eq: val}}`).
 * - Storage validation (`validIdField`): disallowed _id types and `$`-prefixed keys inside object
 *   _id values.
 * - Auto-generated ObjectId when `_id` is omitted; immutability on updates and replacements.
 * - `distinct` projection rules when the key is `_id` vs a dotted path under `_id`.
 * - Wildcard index `wildcardProjection` rules that carve out `_id` (mixed inclusion/exclusion).
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_getmore,
 *   # Time-series collections use a different _id model; this test targets default collections.
 *   exclude_from_timeseries_crud_passthrough,
 *   # Test uses findAndModify
 *   resumable_primary_driven_index_builds_incompatible,
 *   # Test uses commands that cannot be blindly retried.
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   does_not_support_retryable_writes,
 *   # Test does not survive cluster disruptions
 *   does_not_support_stepdowns,
 * ]
 */

import {
    getWinningPlanFromExplain,
    isIdhackOrExpress,
    isClusteredIxscan,
} from "jstests/libs/query/analyze_plan.js";

db.dropDatabase();

const main = db[jsTestName()];
const distinctColl = db[jsTestName() + "_distinct"];
const wildCollInc = db[jsTestName() + "_wildcard_inc"];
const wildCollExc = db[jsTestName() + "_wildcard_exc"];

[main, distinctColl, wildCollInc, wildCollExc].forEach((c) => c.drop());

// --- Query planner: ID hack / express where eligible, and exclusions where not ---

assert.commandWorked(
    main.insertMany([
        {_id: 1, tag: "a"},
        {_id: 2, tag: "b"},
        {_id: {k: "v"}, tag: "obj"},
        {_id: "abc", tag: "regex-target"},
        {_id: 200, tag: "hint-skip"},
    ]),
);

function assertIdFastPath(filter, msg) {
    const explain = main.find(filter).explain();
    const winning = getWinningPlanFromExplain(explain);
    assert(isIdhackOrExpress(db, winning), msg + " explain: " + tojson(winning));
}

function assertNotIdFastPath(filter, msg) {
    const explain = main.find(filter).explain();
    const winning = getWinningPlanFromExplain(explain);
    assert(!isIdhackOrExpress(db, winning), msg + " explain: " + tojson(winning));
}

assertIdFastPath({_id: 1}, "scalar _id equality should use idhack/express");
assertIdFastPath({_id: {$eq: 2}}, "{_id: {$eq}} should use idhack/express");
assertIdFastPath(
    {_id: {k: "v"}},
    "exact object _id (non-operator first key) should use idhack/express",
);

assertNotIdFastPath({_id: {$gte: 0}}, "range on _id should not use idhack/express");
assertNotIdFastPath({_id: /bc/}, "regex on _id should not use idhack/express");

assert.commandWorked(main.createIndex({_id: 1, tag: 1}));
let explainHint = main.find({_id: 200}).hint({_id: 1, tag: 1}).explain();
assert(
    !isIdhackOrExpress(db, getWinningPlanFromExplain(explainHint)),
    "hint should force non-idhack plan: " + tojson(getWinningPlanFromExplain(explainHint)),
);

let explainSkip = main.find({_id: 200}).skip(1).explain();
assert(
    isClusteredIxscan(db, getWinningPlanFromExplain(explainSkip)) ||
        !isIdhackOrExpress(db, getWinningPlanFromExplain(explainSkip)),
    "skip should force non-idhack or clustered ixscan plan: " +
        tojson(getWinningPlanFromExplain(explainSkip)),
);

// --- Aggregation $match on _id ---

assert.eq(main.aggregate([{$match: {_id: 1}}, {$project: {tag: 1, _id: 0}}]).toArray(), [
    {tag: "a"},
]);

// --- Upsert + simple _id query shapes ---

assert.commandWorked(main.updateOne({_id: 42}, {$set: {fromUpsert: 1}}, {upsert: true}));
assert.docEq(main.findOne({_id: 42}), {_id: 42, fromUpsert: 1});

assert.commandWorked(main.updateOne({_id: {$eq: 43}}, {$set: {fromUpsertEq: 1}}, {upsert: true}));
assert.docEq(main.findOne({_id: 43}), {_id: 43, fromUpsertEq: 1});

// --- Auto _id and immutability ---

assert.commandWorked(main.insertOne({noIdField: true}));
let withOid = main.findOne({noIdField: true});
assert(withOid._id instanceof ObjectId, "expected generated ObjectId");

assert.commandFailedWithCode(
    main.update({_id: 1}, {$set: {_id: 99}}, false, false),
    ErrorCodes.ImmutableField,
);

// Replacement update must not change _id (immutable field).
assert.commandFailedWithCode(
    main.update({_id: 2}, {_id: 3, tag: "replaced"}, false, false),
    ErrorCodes.ImmutableField,
);

let fam = main.findAndModify({
    query: {_id: 1},
    update: {$set: {fam: 1}},
    new: true,
});
assert.eq(fam.fam, 1);
assert.eq(fam._id, 1);

// --- validIdField: illegal _id values on insert ---

let bad = db[jsTestName() + "_badid"];
bad.drop();

bad.insert({_id: undefined});
assert.eq(bad.count(), 0);
assert.isnull(bad.exists());

assert.commandFailedWithCode(bad.insert({_id: [1, 2]}), ErrorCodes.InvalidIdField);
assert.commandFailedWithCode(bad.insert({_id: /x/}), ErrorCodes.InvalidIdField);
assert.commandFailedWithCode(bad.insert({_id: {"$bad": 1}}), ErrorCodes.DollarPrefixedFieldName);

// --- distinct: projection special-casing for key `_id` ---

assert.commandWorked(
    distinctColl.insertMany([
        {_id: 1, sortKey: 1},
        {_id: 2, sortKey: 2},
        {_id: {x: 10, y: 20}, sortKey: 3},
        {_id: {x: 11, y: 21}, sortKey: 4},
    ]),
);

let dAll = assert.commandWorked(
    db.runCommand({distinct: distinctColl.getName(), key: "_id", query: {}}),
);
assert.sameMembers(dAll.values, [1, 2, {x: 10, y: 20}, {x: 11, y: 21}]);

let dSub = assert.commandWorked(
    db.runCommand({distinct: distinctColl.getName(), key: "_id.x", query: {}}),
);
assert.sameMembers(dSub.values, [10, 11]);

// --- Wildcard index: `_id` exception for mixed inclusion / exclusion in wildcardProjection ---

assert.commandWorked(
    wildCollInc.createIndex({"$**": 1}, {wildcardProjection: {_id: 0, w: 1}, name: "wild_inc"}),
);
assert.commandWorked(wildCollInc.insertOne({_id: 0, w: "indexed", other: "not-in-proj"}));
assert.eq(wildCollInc.getIndexes().filter((i) => i.name === "wild_inc").length, 1);

assert.commandWorked(
    wildCollExc.createIndex({"$**": 1}, {wildcardProjection: {w: 0, _id: 1}, name: "wild_exc"}),
);
assert.commandWorked(wildCollExc.insertOne({_id: 1, w: "excluded", other: "kept"}));
assert.eq(wildCollExc.getIndexes().filter((i) => i.name === "wild_exc").length, 1);
