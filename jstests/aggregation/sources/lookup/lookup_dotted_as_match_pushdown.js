/**
 * SERVER-126485 regression: when $lookup writes to a dotted 'as' path (e.g. "a.b"), it replaces
 * the parent field "a" entirely. The optimizer must not push a later $match on a sibling
 * sub-path (e.g. "a.d") above the $lookup, because that field no longer exists after the
 * $lookup runs.
 *
 * Reproduces on 8.0, 8.3 and master. Reported against query optimization.
 */
const testDB = db.getSiblingDB("lookup_dotted_as_match_pushdown");
testDB.dropDatabase();

const local = testDB.getCollection("local");
const joined = testDB.getCollection("joined");

// Local doc has 'a' as an array of subdocs containing 'd'. After a $lookup with as: "a.b",
// the parent 'a' is coerced into {b: []}, so 'a.d' no longer exists.
assert.commandWorked(local.insert({_id: 1, x: 5, a: [{d: 5}]}));

// 'joined' is intentionally empty -- the bug is about path rewriting, not the join itself.
assert.commandWorked(joined.insert({_id: "ignored", placeholder: true}));

const pipeline = [
    {$lookup: {from: "joined", as: "a.b", pipeline: [{$match: {nonexistent: true}}]}},
    {$addFields: {"a.c": "$x"}},
    {$match: {"a.d": 5}},
];

// Correct behavior: $match runs AFTER $lookup. By then "a.d" has been erased by the dotted
// 'as' overwrite, so the match returns 0 documents.
// Buggy behavior (pre-fix): optimizer pushes $match before $lookup, sees the original
// {a: [{d: 5}]}, and returns 1 document.
const result = local.aggregate(pipeline).toArray();
assert.eq(result, [], "SERVER-126485: $match on sibling of dotted lookup 'as' must not be " +
                     "pushed before $lookup; expected 0 docs, got " + tojson(result));

// Sanity check: confirm the dotted-'as' overwrite behavior the optimizer must respect.
const afterLookupOnly = local.aggregate([
    {$lookup: {from: "joined", as: "a.b", pipeline: [{$match: {nonexistent: true}}]}},
]).toArray();
assert.eq(afterLookupOnly.length, 1);
assert.eq(afterLookupOnly[0].a, {b: []},
          "dotted 'as' should replace parent 'a' with {b: []}, got " + tojson(afterLookupOnly[0].a));

// Verify the explain plan does not move the $match above the $lookup. The $match stage
// (or its absorbed form) must appear at-or-after $lookup in the optimized pipeline.
const explain = local.explain().aggregate(pipeline);
const stagesStr = tojson(explain);
const lookupIdx = stagesStr.indexOf("$lookup");
const matchIdx = stagesStr.indexOf("a.d");
if (lookupIdx >= 0 && matchIdx >= 0) {
    assert.lt(lookupIdx, matchIdx,
              "SERVER-126485: $match on 'a.d' must not appear before $lookup in optimized " +
              "pipeline. Explain: " + stagesStr);
}
