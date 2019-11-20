/**
 * Tests that the server behaves as expected when an $merge stage is targeting a collection which is
 * involved in the aggregate in some other way, e.g. as the source namespace or via a $lookup.
 *
 * This test issues queries over views, so cannot be run in passthroughs which implicitly shard
 * collections.
 * @tags: [assumes_unsharded_collection]
 */
(function() {
'use strict';

load('jstests/aggregation/extras/merge_helpers.js');  // For 'withEachMergeMode'.
load('jstests/libs/fixture_helpers.js');              // For 'FixtureHelpers'.

const testDB = db.getSiblingDB("merge_to_referenced_coll");
const coll = testDB.test;

// Function used to reset state in between tests.
function reset() {
    coll.drop();
    // Seed the collection to ensure each pipeline will actually do something.
    assert.commandWorked(coll.insert({_id: 0, y: 0}));
}

withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    // Skip the combination of merge modes which will fail depending on the contents of the
    // tested collection.
    if (whenMatchedMode == "fail" || whenNotMatchedMode == "fail")
        return;

    reset();
    // Test $merge to the aggregate command's source collection.
    assert.doesNotThrow(() => coll.aggregate([{
        $merge:
            {into: coll.getName(), whenMatched: whenMatchedMode, whenNotMatched: whenNotMatchedMode}
    }]));

    // Test $merge to the same namespace as a $lookup which is the same as the aggregate
    // command's source collection.
    assert.doesNotThrow(() => coll.aggregate([
        {$lookup: {from: coll.getName(), as: "x", localField: "f_id", foreignField: "_id"}},
        {
            $merge: {
                into: coll.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }
    ]));

    // Test $merge to the same namespace as a $lookup which is *not* the same as the aggregate
    // command's source collection.
    assert.doesNotThrow(() => coll.aggregate([
        {$lookup: {from: "bar", as: "x", localField: "f_id", foreignField: "_id"}},
        {$merge: {into: "bar", whenMatched: whenMatchedMode, whenNotMatched: whenNotMatchedMode}}
    ]));

    // Test $merge to the same namespace as a $graphLookup.
    assert.doesNotThrow(() => coll.aggregate([
            {
              $graphLookup: {
                  from: "bar",
                  startWith: "$_id",
                  connectFromField: "_id",
                  connectToField: "parent_id",
                  as: "graph",
              }
            },
            {
              $merge: {
                  into: "bar",
                  whenMatched: whenMatchedMode,
                  whenNotMatched: whenNotMatchedMode
              }
            }
        ]));

    // Test $merge to the same namespace as a $lookup which is nested within another $lookup.
    assert.doesNotThrow(() => coll.aggregate([
            {
              $lookup: {
                  from: "bar",
                  as: "x",
                  let: {},
                  pipeline: [{$lookup: {from: "TARGET", as: "y", pipeline: []}}]
              }
            },
            {
              $merge: {
                  into: "TARGET",
                  whenMatched: whenMatchedMode,
                  whenNotMatched: whenNotMatchedMode
              }
            }
        ]));
    // Test $merge to the same namespace as a $lookup which is nested within a $facet.
    assert.doesNotThrow(() => coll.aggregate([
        {
            $facet: {
                y: [{$lookup: {from: "TARGET", as: "y", pipeline: []}}],
            }
        },
        {$merge: {into: "TARGET", whenMatched: whenMatchedMode, whenNotMatched: whenNotMatchedMode}}
    ]));
    assert.doesNotThrow(() => coll.aggregate([
        {
            $facet: {
                x: [{$lookup: {from: "other", as: "y", pipeline: []}}],
                y: [{$lookup: {from: "TARGET", as: "y", pipeline: []}}],
            }
        },
        {$merge: {into: "TARGET", whenMatched: whenMatchedMode, whenNotMatched: whenNotMatchedMode}}
    ]));

    // Test that $merge works when the resolved namespace of a view is the same as the output
    // collection.
    assert.commandWorked(
        testDB.runCommand({create: "view_on_TARGET", viewOn: "TARGET", pipeline: []}));
    assert.doesNotThrow(() => testDB.view_on_TARGET.aggregate([
        {$merge: {into: "TARGET", whenMatched: whenMatchedMode, whenNotMatched: whenNotMatchedMode}}
    ]));
    assert.doesNotThrow(() => coll.aggregate([
            {
              $facet: {
                  x: [{$lookup: {from: "other", as: "y", pipeline: []}}],
                  y: [{
                      $lookup: {
                          from: "yet_another",
                          as: "y",
                          pipeline: [{$lookup: {from: "view_on_TARGET", as: "z", pipeline: []}}]
                      }
                  }],
              }
            },
            {
              $merge: {
                  into: "TARGET",
                  whenMatched: whenMatchedMode,
                  whenNotMatched: whenNotMatchedMode
              }
            }
        ]));

    function generateNestedPipeline(foreignCollName, numLevels) {
        let pipeline = [{"$lookup": {pipeline: [], from: foreignCollName, as: "same"}}];

        for (let level = 1; level < numLevels; level++) {
            pipeline = [{"$lookup": {pipeline: pipeline, from: foreignCollName, as: "same"}}];
        }

        return pipeline;
    }

    const nestedPipeline = generateNestedPipeline("lookup", 20).concat([
        {$merge: {into: "lookup", whenMatched: whenMatchedMode, whenNotMatched: whenNotMatchedMode}}
    ]);
    assert.doesNotThrow(() => coll.aggregate(nestedPipeline));

    testDB.dropDatabase();
});
}());
