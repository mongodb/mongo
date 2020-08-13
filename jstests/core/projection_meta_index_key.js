// Test that indexKey $meta projection works in find and aggregate commands and produces correct
// result depending on whether index key metadata is available or not.
// @tags: [
//   sbe_incompatible,
// ]
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For 'isMongos' and 'isSharded'.

const coll = db.projection_meta_index_key;
coll.drop();

assert.commandWorked(
    coll.insert([{_id: 1, a: 10, b: 'x'}, {_id: 2, a: 20, b: 'y'}, {_id: 3, a: 30, b: 'z'}]));

// Appends the given projection 'projSpec' with the {$meta: "indexKey"} expression and ensures
// that it can be applied both in find and aggregate commands. The 'matchSpec' parameters defines
// a query filter to be used in the find command, or in the $match stage of the pipeline. The
// 'indexSpec' is used as a hint to selection of a specific plan. The result of the find command
// is compared against 'expectedResult' parameter, whilst the result of the aggregate against
// 'aggExpectedResult'.
function testIndexKeyMetaProjection({
    matchSpec = {},
    projSpec = {},
    indexSpec = {},
    sortSpec = null,
    expectedResult = [],
    aggExpectedResult = expectedResult
} = {}) {
    projSpec = Object.assign(projSpec, {c: {$meta: "indexKey"}});
    assert.eq(sortSpec ? coll.find(matchSpec, projSpec).sort(sortSpec).hint(indexSpec).toArray()
                       : coll.find(matchSpec, projSpec).hint(indexSpec).toArray(),
              expectedResult);
    assert.eq(coll.aggregate((sortSpec ? [{$sort: sortSpec}] : [])
                                 .concat([{$match: matchSpec}, {$project: projSpec}]),
                             {hint: indexSpec})
                  .toArray(),
              aggExpectedResult);
}

[true, false].forEach((metadataAvailable) => {
    let indexSpec;
    if (metadataAvailable) {
        indexSpec = {a: 1, b: 1};
        assert.commandWorked(coll.createIndex(indexSpec));
    } else {
        indexSpec = {};
        assert.commandWorked(coll.dropIndexes());
    }

    // $meta with an inclusion projection.
    testIndexKeyMetaProjection({
        matchSpec: {a: {$gt: 20}},
        projSpec: {_id: 0, a: 1},
        indexSpec: indexSpec,
        expectedResult: [Object.assign({a: 30}, metadataAvailable ? {c: {a: 30, b: 'z'}} : {})]
    });

    // $meta with an exclusion projection.
    testIndexKeyMetaProjection({
        matchSpec: {a: {$gt: 20}},
        projSpec: {_id: 0, a: 0},
        indexSpec: indexSpec,
        expectedResult: [Object.assign({b: 'z'}, metadataAvailable ? {c: {a: 30, b: 'z'}} : {})]
    });

    // $meta with _id only (inclusion).
    testIndexKeyMetaProjection({
        matchSpec: {a: {$gt: 20}},
        projSpec: {_id: 1},
        indexSpec: indexSpec,
        expectedResult: [Object.assign({_id: 3}, metadataAvailable ? {c: {a: 30, b: 'z'}} : {})]
    });

    // $meta with _id only (exclusion). Note that this type of projection is equivalent to a
    // $meta-only projection, which is treated differently in find and aggregate (it's an inclusion
    // projection in find, and exclusion in aggregate).
    testIndexKeyMetaProjection({
        matchSpec: {a: {$gt: 20}},
        projSpec: {_id: 0},
        indexSpec: indexSpec,
        expectedResult:
            [Object.assign({a: 30, b: 'z'}, metadataAvailable ? {c: {a: 30, b: 'z'}} : {})],
        aggExpectedResult: [metadataAvailable ? {c: {a: 30, b: 'z'}} : {}]
    });

    // $meta only (see comment above regarding $meta-only projection in find and aggregate).
    testIndexKeyMetaProjection({
        matchSpec: {a: {$gt: 20}},
        indexSpec: indexSpec,
        expectedResult:
            [Object.assign({_id: 3, a: 30, b: 'z'}, metadataAvailable ? {c: {a: 30, b: 'z'}} : {})],
        aggExpectedResult: [Object.assign({_id: 3}, metadataAvailable ? {c: {a: 30, b: 'z'}} : {})]
    });

    // $meta with sort (when an index is available this should result in a non-blocking sort
    // and the index key metadata should be available).
    //
    // If a collection is sharded, we will split the pipeline and dispatch only a $project stage,
    // containing inclusion projection, to each shard, and apply the $meta projection on mongos.
    // However, given than no information has been passed to the shard to request to include
    // indexKey metadata to a document, the $meta expression won't be able to extract the
    // indexKey. So, this scenario currently is not supported and we need to make sure that we
    // run this test on an unsharded collection only.
    if (!FixtureHelpers.isSharded(coll)) {
        testIndexKeyMetaProjection({
            projSpec: {_id: 1},
            sortSpec: {a: 1},
            indexSpec: indexSpec,
            expectedResult: [
                Object.assign({_id: 1}, metadataAvailable ? {c: {a: 10, b: 'x'}} : {}),
                Object.assign({_id: 2}, metadataAvailable ? {c: {a: 20, b: 'y'}} : {}),
                Object.assign({_id: 3}, metadataAvailable ? {c: {a: 30, b: 'z'}} : {})
            ]
        });
    }
});
}());
