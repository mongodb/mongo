/**
 * Tests that indexes are distinguished by their "signature", i.e. the combination of parameters
 * which uniquely identify an index. Multiple indexes can be created on the same key pattern if
 * their signature parameters differ.
 *
 * @tags: [
 *   # Asserts on the 'numIndexesAfter' part of the createIndexes command response.
 *   assumes_no_implicit_index_creation,
 *   requires_non_retryable_writes,
 *   # The test that attempts to create an index with partialFilterExpression:
 *   # {a: {$gt: 0, $lt: 10}, b: "BLAH"}} depends on a functionality fixed in 7.3.
 *   requires_fcv_73,
 * ]
 */
import {
    ClusteredCollectionUtil
} from "jstests/libs/clustered_collections/clustered_collection_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDB = db.getSiblingDB(jsTestName());
const coll = testDB.test;
coll.drop();
assert.commandWorked(testDB.createCollection(coll.getName()));

// The key pattern and spec against which other indexes will be compared during createIndex.
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

// Runs a createIndex command with the given key pattern and options. Then verifies that the
// number of indexes changed in accordance with the 'expectedChange' argument.
function buildIndexAndAssertChangeInIndexCount(keyPattern, indexOptions, expectedChange) {
    let numIndexesBefore = coll.getIndexes().length;
    if (ClusteredCollectionUtil.areAllCollectionsClustered(db.getMongo())) {
        // A clustered collection has no actual index on _id even though listIndexes includes
        // an "_id" index in its results with {clustered: true}. See SERVER-59798.
        // This implicit "_id" index however is not included in the createIndexes stats
        // so we need to adjust "numIndexesBefore" to accordingly.
        numIndexesBefore--;
    }

    const cmdRes = assert.commandWorked(coll.createIndex(keyPattern, indexOptions));

    // In a sharded cluster, the results from all shards are returned in cmdRes.raw.
    assert(cmdRes.numIndexesBefore != null || Object.values(cmdRes.raw), tojson(cmdRes));
    const numIndexesAfter =
        (cmdRes.numIndexesAfter != null ? cmdRes.numIndexesAfter
                                        : Object.values(cmdRes.raw)[0].numIndexesAfter);

    assert.eq(numIndexesAfter, numIndexesBefore + expectedChange, cmdRes);
}

// Runs a createIndex command with the given key pattern and options. Then verifies that no index
// was built, since an index with the same signature already existed.
function assertIndexAlreadyExists(keyPattern, indexOptions) {
    buildIndexAndAssertChangeInIndexCount(keyPattern, indexOptions, 0);
}

// Runs a createIndex command with the given key pattern and options. Then verifies that a new
// index was built by checking that the index count increased by 1.
function assertNewIndexBuilt(keyPattern, indexOptions) {
    buildIndexAndAssertChangeInIndexCount(keyPattern, indexOptions, 1);
}

// Creates an index on {a: 1}.
assertNewIndexBuilt(keyPattern, {name: "basic_index"});

// Verifies that we can create a second index on {a: 1} with 'sparse':true.
assertNewIndexBuilt(keyPattern, {name: "sparse_index", sparse: true});

// Verifies that we can create two more indexes with 'unique': true. We do not run these tests on
// sharded passthroughs, since unique indexes cannot be created on a hash-sharded collection.
if (!FixtureHelpers.isSharded(coll)) {
    // Verifies that we can create an index on {a: 1} with 'unique':true.
    assertNewIndexBuilt(keyPattern, {name: "unique_index", unique: true});

    // Verifies that we can create an index on {a: 1} with 'unique':true and 'sparse':true.
    assertNewIndexBuilt(keyPattern, {name: "unique_sparse_index", unique: true, sparse: true});

    assert.commandWorked(coll.dropIndex("unique_sparse_index"));
    assert.commandWorked(coll.dropIndex("unique_index"));
}

assert.commandWorked(coll.dropIndex("basic_index"));
assert.commandWorked(coll.dropIndex("sparse_index"));

// Creates an index on {a: 1} with an explicit collation and a partialFilterExpression.
assertNewIndexBuilt(keyPattern, initialIndexSpec);

// Verifies that an index can be built on the same fields if the collation is different.
assertNewIndexBuilt(keyPattern,
                    makeSpec({name: "simple_collation_index", collation: {locale: "simple"}}));

// Verifies that an index can be built on the same fields if the partialFilterExpression is
// different.
assertNewIndexBuilt(keyPattern, makeSpec({
                        name: "partial_filter_index",
                        partialFilterExpression: {a: {$gt: 5, $lt: 10}, b: "blah"}
                    }));

// Verifies that partialFilterExpressions are normalized before being compared. Below, the
// expression is written differently than in the previous index, but the two are considered
// equivalent. If we attempt to build this index with the same name as the initial index, the
// operation will return success but will not actually do any work, since the requested index
// already exists.
const partialFilterDupeSpec =
    makeSpec({partialFilterExpression: {$and: [{b: "blah"}, {a: {$lt: 10}}, {a: {$gt: 0}}]}});
assertIndexAlreadyExists(keyPattern, partialFilterDupeSpec);

// Verifies that attempting to build the dupe index with a different name will result in an error.
partialFilterDupeSpec.name = "partial_filter_dupe_index";
assert.commandFailedWithCode(coll.createIndex(keyPattern, partialFilterDupeSpec),
                             ErrorCodes.IndexOptionsConflict);

// We take collation into account when checking partialFilterExpression equivalence.
// In this instance we are using a case-insensitive collation, and so the predicate {b: "BLAH"} will
// match the same set of documents as {b: "blah"} in the initial index's partialFilterExpression.
assertIndexAlreadyExists(
    keyPattern,
    makeSpec({name: "initial_index", partialFilterExpression: {a: {$gt: 0, $lt: 10}, b: "BLAH"}}));

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

// Changes the predicate order of the $and and re-run the createIndex. We would expect this index to
// be considered identical to the existing index, and for the createIndex to return no-op success.
// Instead, we throw an exception because the catalog believes we are trying to create an index with
// the same name but a different partialFilterExpression.
partialFilterUnsortedLeaves.partialFilterExpression.$and.reverse();
assert.commandFailedWithCode(coll.createIndex(keyPattern, partialFilterUnsortedLeaves),
                             ErrorCodes.IndexKeySpecsConflict);

// Verifies that non-signature options cannot distinguish a new index from an existing index.
const nonSignatureOptions = [{expireAfterSeconds: 10}];

// Builds a new, basic index on {a: 1}, since some of the options we intend to test are not
// compatible with the partialFilterExpression on the existing {a: 1} indexes.
assertNewIndexBuilt(keyPattern, {name: "basic_index_default_opts"});

// Verifies that none of the options in the list are sufficient to uniquely identify an index,
// meaning that we cannot create a new index on 'keyPattern' by changing any of these fields.
for (let nonSigOpt of nonSignatureOptions) {
    assert.commandFailedWithCode(
        coll.createIndex(keyPattern, Object.assign({name: "non_sig_index"}, nonSigOpt)),
        ErrorCodes.IndexOptionsConflict);
}

const wildcardKeyPattern = {
    "$**": 1
};

// Builds a base wildcard index.
assertNewIndexBuilt(wildcardKeyPattern, {name: "wc_all"});

// Verifies that two indexes which includes or excludes a same field can be created.
assertNewIndexBuilt(wildcardKeyPattern, {name: "wc_a", wildcardProjection: {a: 1}});
assertNewIndexBuilt(wildcardKeyPattern, {name: "wc_noa", wildcardProjection: {a: 0}});

// Verifies the behavior that _id is excluded by default and so the {_id: 0, a: 1} path projection
// equals to the {a: 1} path projection and thus the index with {_id: 0, a: 1} path projection can
// not be created.
assertIndexAlreadyExists(wildcardKeyPattern, {name: "wc_a", wildcardProjection: {_id: 0, a: 1}});
assert.commandFailedWithCode(
    coll.createIndex(wildcardKeyPattern, {name: "wc_noid_a", wildcardProjection: {_id: 0, a: 1}}),
    ErrorCodes.IndexOptionsConflict);

// Verifies that the index with the {_id: 1, a: 1} path projection has the different index signature
// from the {a: 1} path projection and thus can be created.
assertNewIndexBuilt(wildcardKeyPattern, {name: "wc_id_a", wildcardProjection: {_id: 1, a: 1}});

// Verifies that the {_id: 0, a: 0} path projection is same as {a: 0} and thus an index with the
// projection can not be created.
assertIndexAlreadyExists(wildcardKeyPattern, {name: "wc_noa", wildcardProjection: {_id: 0, a: 0}});
assert.commandFailedWithCode(
    coll.createIndex(wildcardKeyPattern, {name: "wc_noid_noa", wildcardProjection: {_id: 0, a: 0}}),
    ErrorCodes.IndexOptionsConflict);

// Verifies that the {a: 0, _id: 1} path projection is different from {a: 0} and an index with the
// projection can be created.
assertNewIndexBuilt(wildcardKeyPattern, {name: "wc_noa_id", wildcardProjection: {a: 0, _id: 1}});

// Verifies that an index with sub fields for a field which is included in another wildcard path
// projection can be created.
assertNewIndexBuilt(wildcardKeyPattern,
                    {name: "wc_a_sub_b_c", wildcardProjection: {"a.b": 1, "a.c": 1}});

// Verifies that indexes with a path projection which is identical after normalization can not be
// created.
assertIndexAlreadyExists(wildcardKeyPattern,
                         {name: "wc_a_sub_b_c", wildcardProjection: {a: {b: 1, c: 1}}});
assert.commandFailedWithCode(
    coll.createIndex(wildcardKeyPattern,
                     {name: "wc_a_sub_b_c_1", wildcardProjection: {a: {b: 1, c: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assertIndexAlreadyExists(wildcardKeyPattern,
                         {name: "wc_a_sub_b_c", wildcardProjection: {a: {c: 1, b: 1}}});
assert.commandFailedWithCode(
    coll.createIndex(wildcardKeyPattern,
                     {name: "wc_a_sub_b_c_1", wildcardProjection: {a: {c: 1, b: 1}}}),
    ErrorCodes.IndexOptionsConflict);
assertIndexAlreadyExists(wildcardKeyPattern,
                         {name: "wc_a_sub_b_c", wildcardProjection: {"a.c": 1, "a.b": 1}});
assert.commandFailedWithCode(
    coll.createIndex(wildcardKeyPattern,
                     {name: "wc_a_sub_b_c_1", wildcardProjection: {"a.c": 1, "a.b": 1}}),
    ErrorCodes.IndexOptionsConflict);

const compoundWildcardIndex = {
    "$**": 1,
    'other': 1
};

// Verifies that two indexes which includes or excludes a same field can be created.
assertNewIndexBuilt(compoundWildcardIndex, {name: "cwi_a", wildcardProjection: {a: 1}});
assertNewIndexBuilt(compoundWildcardIndex, {name: "cwi_noa", wildcardProjection: {a: 0, other: 0}});

// Verifies that the {_id: 0, a: 0} path projection is same as {a: 0} and thus an index with the
// projection can not be created.
assertIndexAlreadyExists(compoundWildcardIndex,
                         {name: "cwi_noa", wildcardProjection: {_id: 0, a: 0, other: 0}});

assertNewIndexBuilt(compoundWildcardIndex,
                    {name: "cwi_a_sub_b_c", wildcardProjection: {"a.b": 1, "a.c": 1}});

assertIndexAlreadyExists(compoundWildcardIndex,
                         {name: "cwi_a_sub_b_c", wildcardProjection: {"a.b": 1, "a.c": 1}});
assert.commandFailedWithCode(
    coll.createIndex(compoundWildcardIndex,
                     {name: "cwi_a_sub_b_c_1", wildcardProjection: {"a.b": 1, "a.c": 1}}),
    ErrorCodes.IndexOptionsConflict);
assertIndexAlreadyExists(compoundWildcardIndex,
                         {name: "cwi_a_sub_b_c", wildcardProjection: {"a.c": 1, "a.b": 1}});
assert.commandFailedWithCode(
    coll.createIndex(compoundWildcardIndex,
                     {name: "cwi_a_sub_b_c_1", wildcardProjection: {"a.c": 1, "a.b": 1}}),
    ErrorCodes.IndexOptionsConflict);
