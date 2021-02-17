/**
 * Tests that indexes are distinguished by their "signature", i.e. the combination of parameters
 * which uniquely identify an index. Multiple indexes can be created on the same key pattern if
 * their signature parameters differ.
 *
 * @tags: [requires_fcv_49, requires_non_retryable_writes]
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For 'isSharded'.

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.test;
coll.drop();
assert.commandWorked(testDB.createCollection(coll.getName()));

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

// Runs a createIndexes command with the given key pattern and options. Then verifies that the
// number of indexes changed in accordance with the 'expectedChange' argument.
function buildIndexAndAssertChangeInIndexCount(keyPattern, indexOptions, expectedChange) {
    const numIndexesBefore = coll.getIndexes().length;
    const cmdRes = assert.commandWorked(coll.createIndex(keyPattern, indexOptions));

    // In a sharded cluster, the results from all shards are returned in cmdRes.raw.
    assert(cmdRes.numIndexesBefore != null || Object.values(cmdRes.raw), tojson(cmdRes));
    const numIndexesAfter =
        (cmdRes.numIndexesAfter != null ? cmdRes.numIndexesAfter
                                        : Object.values(cmdRes.raw)[0].numIndexesAfter);

    assert.eq(numIndexesAfter, numIndexesBefore + expectedChange, cmdRes);
}

// Runs a createIndexes command with the given key pattern and options. Then verifies that no index
// was built, since an index with the same signature already existed.
function assertIndexAlreadyExists(keyPattern, indexOptions) {
    buildIndexAndAssertChangeInIndexCount(keyPattern, indexOptions, 0);
}

// Runs a createIndexes command with the given key pattern and options. Then verifies that a new
// index was built by checking that the index count increased by 1.
function assertNewIndexBuilt(keyPattern, indexOptions) {
    buildIndexAndAssertChangeInIndexCount(keyPattern, indexOptions, 1);
}

// Create an index on {a: 1}.
assertNewIndexBuilt(keyPattern, {name: "basic_index"});

// Verify that we can create a second index on {a: 1} with sparse:true.
assertNewIndexBuilt(keyPattern, {name: "sparse_index", sparse: true});

// Verify that we can create two more indexex with 'unique': true. We do not run these tests on
// sharded passthroughs, since unique indexes cannot be created on a hash-sharded collection.
if (!FixtureHelpers.isSharded(coll)) {
    // Verify that we can create an index on {a: 1} with unique:true.
    assertNewIndexBuilt(keyPattern, {name: "unique_index", unique: true});

    // Verify that we can create an index on {a: 1} with unique:true and sparse:true.
    assertNewIndexBuilt(keyPattern, {name: "unique_sparse_index", unique: true, sparse: true});

    assert.commandWorked(coll.dropIndex("unique_sparse_index"));
    assert.commandWorked(coll.dropIndex("unique_index"));
}

assert.commandWorked(coll.dropIndex("basic_index"));
assert.commandWorked(coll.dropIndex("sparse_index"));

// Create an index on {a: 1} with an explicit collation and a partial filter expression.
assertNewIndexBuilt(keyPattern, initialIndexSpec);

// Verify that an index can be built on the same fields if the collation is different.
assertNewIndexBuilt(keyPattern,
                    makeSpec({name: "simple_collation_index", collation: {locale: "simple"}}));

// Verify that an index can be built on the same fields if the partialFilterExpression is different.
assertNewIndexBuilt(keyPattern, makeSpec({
                        name: "partial_filter_index",
                        partialFilterExpression: {a: {$gt: 5, $lt: 10}, b: "blah"}
                    }));

// Verify that partialFilterExpressions are normalized before being compared. Below, the expression
// is written differently than in the previous index, but the two are considered equivalent. If we
// attempt to build this index with the same name as the initial index, the operation will return
// success but will not actually do any work, since the requested index already exists.
const partialFilterDupeSpec =
    makeSpec({partialFilterExpression: {$and: [{b: "blah"}, {a: {$lt: 10}}, {a: {$gt: 0}}]}});
assertIndexAlreadyExists(keyPattern, partialFilterDupeSpec);

// Verify that attempting to build the dupe index with a different name will result in an error.
partialFilterDupeSpec.name = "partial_filter_dupe_index";
assert.commandFailedWithCode(coll.createIndex(keyPattern, partialFilterDupeSpec),
                             ErrorCodes.IndexOptionsConflict);

// We don't currently take collation into account when checking partialFilterExpression equivalence.
// In this instance we are using a case-insensitive collation, and so the predicate {b: "BLAH"} will
// match the same set of documents as {b: "blah"} in the initial index's partialFilterExpression.
// But we do not consider these filters equivalent, and so this is considered a distinct index.
// TODO SERVER-47664: take collation into account in MatchExpression::equivalent().
assertNewIndexBuilt(keyPattern, makeSpec({
                        name: "partial_filter_collator_index",
                        partialFilterExpression: {a: {$gt: 0, $lt: 10}, b: "BLAH"}
                    }));

// We do not currently sort MatchExpression trees by leaf predicate value in cases where two or more
// branches are otherwise identical, meaning that we cannot identify certain trivial cases where two
// partialFilterExpressions are equivalent.
// TODO SERVER-47661: provide a way for MatchExpression trees to be sorted in a consistent manner in
// cases where two or more otherwise identical branches differ only by leaf predicate value.
const partialFilterUnsortedLeaves = makeSpec({
    name: "partial_filter_single_field_multiple_predicates_same_matchtype",
    partialFilterExpression: {$and: [{a: {$type: 1}}, {a: {$type: 2}}]}
});
assertNewIndexBuilt(keyPattern, partialFilterUnsortedLeaves);

// Change the predicate order of the $and and re-run the createIndex. We would expect this index to
// be considered identical to the existing index, and for the createIndex to return no-op success.
// Instead, we throw an exception because the catalog believes we are trying to create an index with
// the same name but a different partialFilterExpression.
partialFilterUnsortedLeaves.partialFilterExpression.$and.reverse();
assert.commandFailedWithCode(coll.createIndex(keyPattern, partialFilterUnsortedLeaves),
                             ErrorCodes.IndexKeySpecsConflict);

// Verify that non-signature options cannot distinguish a new index from an existing index.
const nonSignatureOptions = [{expireAfterSeconds: 10}, {background: true}];

// Build a new, basic index on {a: 1}, since some of the options we intend to test are not
// compatible with the partialFilterExpression on the existing {a: 1} indexes.
assertNewIndexBuilt(keyPattern, {name: "basic_index_default_opts"});

// Verify that none of the options in the list are sufficient to uniquely identify an index, meaning
// that we cannot create a new index on 'keyPattern' by changing any of these fields.
for (let nonSigOpt of nonSignatureOptions) {
    assert.commandFailedWithCode(
        coll.createIndex(keyPattern, Object.assign({name: "non_sig_index"}, nonSigOpt)),
        ErrorCodes.IndexOptionsConflict);
}

// Build a new index on {$**: 1} and verify that wildcardProjection is a non-signature field.
// TODO SERVER-47659: wildcardProjection should be part of the signature.
assertNewIndexBuilt({"$**": 1}, {name: "wildcard_index_default_opts"});
assert.commandFailedWithCode(coll.createIndex({"$**": 1}, {wildcardProjection: {a: 1}}),
                             ErrorCodes.IndexOptionsConflict);
})();
