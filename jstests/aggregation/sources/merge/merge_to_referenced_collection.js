/**
 * Tests that the server behaves as expected when an $merge stage is targeting a collection which is
 * involved in the aggregate in some other way, e.g. as the source namespace or via a $lookup. We
 * disallow this combination in an effort to prevent the "halloween problem" of a never-ending
 * query.
 *
 * This test issues queries over views, so cannot be run in passthroughs which implicitly shard
 * collections.
 * @tags: [assumes_unsharded_collection]
 */
(function() {
    'use strict';

    load('jstests/aggregation/extras/out_helpers.js');  // For 'withEachMergeMode'.
    load('jstests/libs/fixture_helpers.js');            // For 'FixtureHelpers'.

    const testDB = db.getSiblingDB("merge_to_referenced_coll");
    const coll = testDB.test;

    withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
        coll.drop();

        // Seed the collection to ensure each pipeline will actually do something.
        assert.commandWorked(coll.insert({_id: 0}));

        // Each of the following assertions will somehow use $merge to write to a namespace that is
        // being read from elsewhere in the pipeline.
        const assertFailsWithCode = ((fn) => {
            const error = assert.throws(fn);
            assert.contains(error.code, [51188, 51079]);
        });

        // Test $merge to the aggregate command's source collection.
        assertFailsWithCode(() => coll.aggregate([{
            $merge: {
                into: coll.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]));

        // Test $merge to the same namespace as a $lookup which is the same as the aggregate
        // command's source collection.
        assertFailsWithCode(() => coll.aggregate([
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
        assertFailsWithCode(() => coll.aggregate([
            {$lookup: {from: "bar", as: "x", localField: "f_id", foreignField: "_id"}},
            {
              $merge: {
                  into: "bar",
                  whenMatched: whenMatchedMode,
                  whenNotMatched: whenNotMatchedMode
              }
            }
        ]));

        // Test $merge to the same namespace as a $graphLookup.
        assertFailsWithCode(() => coll.aggregate([
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
        assertFailsWithCode(() => coll.aggregate([
            {
              $lookup: {
                  from: "bar",
                  as: "x",
                  let : {},
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
        assertFailsWithCode(() => coll.aggregate([
            {
              $facet: {
                  y: [{$lookup: {from: "TARGET", as: "y", pipeline: []}}],
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
        assertFailsWithCode(() => coll.aggregate([
            {
              $facet: {
                  x: [{$lookup: {from: "other", as: "y", pipeline: []}}],
                  y: [{$lookup: {from: "TARGET", as: "y", pipeline: []}}],
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

        // Test that we use the resolved namespace of a view to detect this sort of halloween
        // problem.
        assert.commandWorked(
            testDB.runCommand({create: "view_on_TARGET", viewOn: "TARGET", pipeline: []}));
        assertFailsWithCode(() => testDB.view_on_TARGET.aggregate([{
            $merge: {
                into: "TARGET",
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]));
        assertFailsWithCode(() => coll.aggregate([
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

        const nestedPipeline = generateNestedPipeline("lookup", 20).concat([{
            $merge: {
                into: "lookup",
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode
            }
        }]);
        assertFailsWithCode(() => coll.aggregate(nestedPipeline));

        testDB.dropDatabase();
    });
}());
