/**
 * Verify the error behavior of '$$SEARCH_META' when it is not properly configured. $$SEARCH_META
 * throws if used in aggregations on sharded collections.
 * @tags: [
 *   # $search/$searchMeta cannot be used within a facet
 *   do_not_wrap_aggregations_in_facets,
 *   # $search/$searchMeta do not support any read concern other than "local"
 *   assumes_read_concern_unchanged
 * ]
 */
(function() {
"use strict";

const coll = db.searchCollector;
coll.drop();
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

// Check that a query without a search stage gets errors if SEARCH_META is accessed.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: coll.getName(),
        cursor: {},
        pipeline: [{$project: {_id: 1, meta: "$$SEARCH_META"}}]
    }),
    [6347902, 6347903]);  // Error code depends on presence of the enterprise module.

// Check that users cannot assign values to SEARCH_META.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$lookup: {from: coll.getName(), let : {SEARCH_META: "$title"}, as: "joined", pipeline: []}}
    ],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);

const response = db.runCommand({
    aggregate: "non_existent_namespace",
    pipeline: [{$searchMeta: {query: {nonsense: true}}}],
    cursor: {}
});
if (!response.ok) {
    assert.commandFailedWithCode(response, [31082, 40324] /* community or mongos */);
} else {
    assert.eq(response.cursor.firstBatch, []);
}
})();
