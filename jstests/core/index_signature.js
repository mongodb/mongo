/**
 * Tests that indexes are distinguished by their "signature", i.e. the combination of parameters
 * which uniquely identify an index. Multiple indexes can be created on the same key pattern if
 * their signature parameters differ.
 *
 * TODO SERVER-46592: This test is multiversion-incompatible in 4.6. If we use 'requires_fcv_46'
 *                    as the tag for that, removing 'requires_fcv_44' is sufficient. Otherwise,
 *                    please set the appropriate tag when removing 'requires_fcv_44'
 * @tags: [requires_fcv_44, requires_fcv_46, requires_non_retryable_writes]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.test;
coll.drop();

// The key pattern and spec against which other indexes will be compared during createIndexes.
const initialIndexSpec = {
    name: "initial_index",
    collation: {locale: "en_US", strength: 1},
    partialFilterExpression: {a: {$gt: 0, $lt: 10}, b: "blah"}
};
const keyPattern = {
    a: 1
};

// Helper function to build an index spec based on 'initialIndexSpec'.
function makeSpec(opts) {
    return Object.assign({}, initialIndexSpec, opts || {});
}

// Verifies that the number of indexes changed in accordance with the 'expectedChange' argument.
function assertNumIndexesAfterComparedToBefore(cmdRes, expectedChange) {
    // In a sharded cluster, the results from all shards are returned in cmdRes.raw.
    assert(cmdRes.numIndexesBefore != null || Object.values(cmdRes.raw), tojson(cmdRes));
    const numIndexesBefore =
        (cmdRes.numIndexesBefore != null ? cmdRes.numIndexesBefore
                                         : Object.values(cmdRes.raw)[0].numIndexesBefore);
    const numIndexesAfter =
        (cmdRes.numIndexesAfter != null ? cmdRes.numIndexesAfter
                                        : Object.values(cmdRes.raw)[0].numIndexesAfter);
    assert.eq(numIndexesAfter, numIndexesBefore + expectedChange);
}

// Create an index on {a: 1} with an explicit collation and a partial filter expression.
let cmdRes = assert.commandWorked(coll.createIndex(keyPattern, initialIndexSpec));
assertNumIndexesAfterComparedToBefore(cmdRes, 1);

// Verify that an index can be built on the same fields if the collation is different.
cmdRes = assert.commandWorked(coll.createIndex(
    keyPattern, makeSpec({name: "simple_collation_index", collation: {locale: "simple"}})));
assertNumIndexesAfterComparedToBefore(cmdRes, 1);

// Verify that an index can be built on the same fields if the partialFilterExpression is different.
cmdRes = assert.commandWorked(
    coll.createIndex(keyPattern, makeSpec({
                         name: "partial_filter_index",
                         partialFilterExpression: {a: {$gt: 5, $lt: 10}, b: "blah"}
                     })));
assertNumIndexesAfterComparedToBefore(cmdRes, 1);

// Verify that partialFilterExpressions are normalized before being compared. Below, the expression
// is written differently than in the previous index, but the two are considered equivalent. If we
// attempt to build this index with the same name as the initial index, the operation will return
// success but will not actually do any work, since the requested index already exists.
const partialFilterDupeSpec =
    makeSpec({partialFilterExpression: {$and: [{b: "blah"}, {a: {$lt: 10}}, {a: {$gt: 0}}]}});
cmdRes = assert.commandWorked(coll.createIndex(keyPattern, partialFilterDupeSpec));
assertNumIndexesAfterComparedToBefore(cmdRes, 0);

// Verify that attempting to build the dupe index with a different name will result in an error.
partialFilterDupeSpec.name = "partial_filter_dupe_index";
assert.commandFailedWithCode(coll.createIndex(keyPattern, partialFilterDupeSpec),
                             ErrorCodes.IndexOptionsConflict);

// We don't currently take collation into account when checking partialFilterExpression equivalence.
// In this instance we are using a case-insensitive collation, and so the predicate {b: "BLAH"} will
// match the same set of documents as {b: "blah"} in the initial index's partialFilterExpression.
// But we do not consider these filters equivalent, and so this is considered a distinct index.
// TODO SERVER-47664: take collation into account in MatchExpression::equivalent().
cmdRes = assert.commandWorked(
    coll.createIndex(keyPattern, makeSpec({
                         name: "partial_filter_collator_index",
                         partialFilterExpression: {a: {$gt: 0, $lt: 10}, b: "BLAH"}
                     })));
assertNumIndexesAfterComparedToBefore(cmdRes, 1);

// We do not currently sort MatchExpression trees by leaf predicate value in cases where two or more
// branches are otherwise identical, meaning that we cannot identify certain trivial cases where two
// partialFilterExpressions are equivalent.
// TODO SERVER-47661: provide a way for MatchExpression trees to be sorted in a consistent manner in
// cases where two or more otherwise identical branches differ only by leaf predicate value.
const partialFilterUnsortedLeaves = makeSpec({
    name: "partial_filter_single_field_multiple_predicates_same_matchtype",
    partialFilterExpression: {$and: [{a: {$type: 1}}, {a: {$type: 2}}]}
});
cmdRes = assert.commandWorked(coll.createIndex(keyPattern, partialFilterUnsortedLeaves));
assertNumIndexesAfterComparedToBefore(cmdRes, 1);

// Change the predicate order of the $and and re-run the createIndex. We would expect this index to
// be considered identical to the existing index, and for the createIndex to return no-op success.
// Instead, we throw an exception because the catalog believes we are trying to create an index with
// the same name but a different partialFilterExpression.
partialFilterUnsortedLeaves.partialFilterExpression.$and.reverse();
assert.commandFailedWithCode(coll.createIndex(keyPattern, partialFilterUnsortedLeaves),
                             ErrorCodes.IndexKeySpecsConflict);

// Verify that non-signature options cannot distinguish a new index from an existing index.
// TODO SERVER-47657: unique and sparse should be part of the signature.
const nonSignatureOptions =
    [{unique: true}, {sparse: true}, {expireAfterSeconds: 10}, {background: true}];

// Build a new, basic index on {a: 1}, since some of the options we intend to test are not
// compatible with the partialFilterExpression on the existing {a: 1} indexes.
cmdRes = assert.commandWorked(coll.createIndex(keyPattern, {name: "basic_index_default_opts"}));
assertNumIndexesAfterComparedToBefore(cmdRes, 1);

// Verify that none of the options in the list are sufficient to uniquely identify an index, meaning
// that we cannot create a new index on 'keyPattern' by changing any of these fields.
for (let nonSigOpt of nonSignatureOptions) {
    assert.commandFailedWithCode(
        coll.createIndex(keyPattern, Object.assign({name: "non_sig_index"}, nonSigOpt)),
        ErrorCodes.IndexOptionsConflict);
}

// Build a new index on {$**: 1} and verify that wildcardProjection is a non-signature field.
// TODO SERVER-47659: wildcardProjection should be part of the signature.
cmdRes = assert.commandWorked(coll.createIndex({"$**": 1}, {name: "wildcard_index_default_opts"}));
assertNumIndexesAfterComparedToBefore(cmdRes, 1);
assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {wildcardProjection: {a: 1}}),
                             ErrorCodes.IndexOptionsConflict);
})();
