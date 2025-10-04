/**
 * Tests the autoparameterization of the SBE cached plan that includes positional projection.
 *  @tags: [
 *    requires_getmore,
 *    # Positional projection is not supported on views.
 *    incompatible_with_views,
 *  ]
 */
const coll = db.plan_cache_positional_projection;
coll.drop();
const nonMatchingDoc = {
    "c": 3,
};
const matchingDoc1 = {
    "c": 2,
    "a": {"b": 3},
};
const matchingDoc2 = {
    "c": 2,
    "a": {"b": 1},
};
const arr = {
    "arr": [nonMatchingDoc, matchingDoc1, matchingDoc2],
};
assert.commandWorked(coll.insert(arr));

// Run a query which can populate the plan cache.
assert.eq(0, coll.find({"arr": {"$elemMatch": {"c": 99, "a.b": {"$exists": true}}}}, {"arr.$": 1}).itcount());

// Run the same query but with different constants. This query can use the plan cache entry created
// earlier.
const result = coll.find({"arr": {"$elemMatch": {"c": 2, "a.b": {"$exists": true}}}}, {"arr.$": 1, "_id": 0}).toArray();
assert.eq([{arr: [matchingDoc1]}], result, result);
