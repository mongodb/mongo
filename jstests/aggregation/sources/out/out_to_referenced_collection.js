// Tests that the server behaves as expected when an $out stage is targeting a collection which is
// involved in the aggregate in some other way, e.g. as the source namespace or via a $lookup. We
// disallow this combination in an effort to prevent the "halloween problem" of a never-ending
// query. If the $out is using mode "replaceCollection" then this is legal because we use a
// temporary collection. If the out is using any other mode which would be "in place", we expect the
// server to error in an effort to prevent server-side infinite loops.
// This test issues queries over views, so cannot be run in passthroughs which implicitly shard
// collections.
// @tags: [assumes_unsharded_collection]
(function() {
    'use strict';

    load('jstests/aggregation/extras/out_helpers.js');  // For 'withEachOutMode'.
    load('jstests/libs/fixture_helpers.js');            // For 'FixtureHelpers'.

    const testDB = db.getSiblingDB("out_to_referenced_coll");
    const coll = testDB.test;

    withEachOutMode(mode => {
        coll.drop();
        if (FixtureHelpers.isSharded(coll) && mode === "replaceCollection") {
            return;  // Not a supported combination.
        }

        // Seed the collection to ensure each pipeline will actually do something.
        assert.commandWorked(coll.insert({_id: 0}));

        // Each of the following assertions will somehow use $out to write to a namespace that is
        // being read from elsewhere in the pipeline. This is legal with mode "replaceCollection".
        const assertFailsWithCode = ((fn) => {
            const error = assert.throws(fn);
            assert.contains(error.code, [50992, 51079]);
        });
        const asserter = (mode === "replaceCollection") ? assert.doesNotThrow : assertFailsWithCode;

        // Test $out to the aggregate command's source collection.
        asserter(() => coll.aggregate([{$out: {to: coll.getName(), mode: mode}}]));
        // Test $out to the same namespace as a $lookup which is the same as the aggregate command's
        // source collection.
        asserter(() => coll.aggregate([
            {$lookup: {from: coll.getName(), as: "x", localField: "f_id", foreignField: "_id"}},
            {$out: {to: coll.getName(), mode: mode}}
        ]));
        // Test $out to the same namespace as a $lookup which is *not* the same as the aggregate
        // command's source collection.
        asserter(() => coll.aggregate([
            {$lookup: {from: "bar", as: "x", localField: "f_id", foreignField: "_id"}},
            {$out: {to: "bar", mode: mode}}
        ]));
        // Test $out to the same namespace as a $graphLookup.
        asserter(() => coll.aggregate([
            {
              $graphLookup: {
                  from: "bar",
                  startWith: "$_id",
                  connectFromField: "_id",
                  connectToField: "parent_id",
                  as: "graph",
              }
            },
            {$out: {to: "bar", mode: mode}}
        ]));
        // Test $out to the same namespace as a $lookup which is nested within another $lookup.
        asserter(() => coll.aggregate([
            {
              $lookup: {
                  from: "bar",
                  as: "x",
                  let : {},
                  pipeline: [{$lookup: {from: "TARGET", as: "y", pipeline: []}}]
              }
            },
            {$out: {to: "TARGET", mode: mode}}
        ]));
        // Test $out to the same namespace as a $lookup which is nested within a $facet.
        asserter(() => coll.aggregate([
            {
              $facet: {
                  y: [{$lookup: {from: "TARGET", as: "y", pipeline: []}}],
              }
            },
            {$out: {to: "TARGET", mode: mode}}
        ]));
        asserter(() => coll.aggregate([
            {
              $facet: {
                  x: [{$lookup: {from: "other", as: "y", pipeline: []}}],
                  y: [{$lookup: {from: "TARGET", as: "y", pipeline: []}}],
              }
            },
            {$out: {to: "TARGET", mode: mode}}
        ]));

        // Test that we use the resolved namespace of a view to detect this sort of halloween
        // problem.
        assert.commandWorked(
            testDB.runCommand({create: "view_on_TARGET", viewOn: "TARGET", pipeline: []}));
        asserter(() => testDB.view_on_TARGET.aggregate([{$out: {to: "TARGET", mode: mode}}]));
        asserter(() => coll.aggregate([
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
            {$out: {to: "TARGET", mode: mode}}
        ]));

        function generateNestedPipeline(foreignCollName, numLevels) {
            let pipeline = [{"$lookup": {pipeline: [], from: foreignCollName, as: "same"}}];

            for (let level = 1; level < numLevels; level++) {
                pipeline = [{"$lookup": {pipeline: pipeline, from: foreignCollName, as: "same"}}];
            }

            return pipeline;
        }

        const nestedPipeline =
            generateNestedPipeline("lookup", 20).concat([{$out: {to: "lookup", mode: mode}}]);
        asserter(() => coll.aggregate(nestedPipeline));

        testDB.dropDatabase();  // Clear any of the collections which would be created by the
                                // successful "replaceCollection" mode of this test.
    });
}());
