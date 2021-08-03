/**
 * Verify the error behavior of '$$SEARCH_META' when it is not properly configured. $$SEARCH_META
 * throws if used in aggregations on sharded collections.
 * @tags: [
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";
const getSearchMetaParam = db.adminCommand({getParameter: 1, featureFlagSearchMeta: 1});
const isSearchMetaEnabled = getSearchMetaParam.hasOwnProperty("featureFlagSearchMeta") &&
    getSearchMetaParam.featureFlagSearchMeta.value;
if (!isSearchMetaEnabled) {
    return;
}

const coll = db.searchCollector;
coll.drop();
assert.commandWorked(coll.insert({"_id": 1, "title": "cakes"}));

// Check that a query without a search stage gets a missing value if SEARCH_META is accessed.
const result = coll.aggregate([{$project: {_id: 1, meta: "$$SEARCH_META"}}]).toArray();
assert.eq(result.length, 1);
assert.eq(result[0], {_id: 1});

// Check that users cannot assign values to SEARCH_META.
assert.commandFailedWithCode(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [
        {$lookup: {from: coll.getName(), let : {SEARCH_META: "$title"}, as: "joined", pipeline: []}}
    ],
    cursor: {}
}),
                             ErrorCodes.FailedToParse);
})();
