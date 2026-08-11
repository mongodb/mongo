/**
 * Tests how desugaring extension stages interact with pipeline-length and view-depth limits when
 * reached through $lookup and $graphLookup.
 *
 * A desugaring stage expands during view resolution, so a pipeline within the length limit as
 * written can exceed it afterwards. Long-but-legal pipelines must succeed, and the view-depth limit
 * must be enforced the same way whether or not desugaring extensions are involved.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionStubParsers,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_90,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";

const localCollName = jsTestName() + "_local";
const foreignCollName = jsTestName() + "_foreign";

// Longest view chain that can actually be *resolved*: ViewGraph::kMaxViewDepth is 20, but the
// resolution loop in view_catalog_helpers.cpp:181 spends one iteration on the terminal non-view
// collection, so a 20-view chain can be created yet not resolved. Pre-existing server behavior,
// unrelated to extensions.
const kMaxViewDepth = 19;
const kViewDepthExceededCode = ErrorCodes.ViewDepthLimitExceeded;

describe("desugaring extensions and pipeline size limits", function () {
    let localColl, foreignColl;
    let createdViews;

    before(function () {
        localColl = db[localCollName];
        foreignColl = db[foreignCollName];
        localColl.drop();
        foreignColl.drop();
        assert.commandWorked(localColl.insertMany([{_id: 1, key: "A"}]));
        assert.commandWorked(
            foreignColl.insertMany([
                {_id: 10, name: "x", val: 100},
                {_id: 11, name: "y", val: 200},
            ]),
        );
        createdViews = [];
    });

    after(function () {
        // Drop in reverse creation order so a view is removed before the view it depends on.
        for (const viewName of createdViews.reverse()) {
            assertDropCollection(db, viewName);
        }
        createdViews = [];
    });

    function makeView(name, source, pipeline) {
        assert.commandWorked(db.createView(name, source, pipeline));
        createdViews.push(name);
        return name;
    }

    it("$lookup succeeds on a view whose definition is many desugaring stages long", function () {
        // 25 $matchTopN stages expand to 75, well under internalPipelineLengthLimit, so this must
        // succeed even though the limit is checked against the expanded count.
        const kNumStages = 25;
        const viewPipeline = [];
        for (let i = 0; i < kNumStages; i++) {
            viewPipeline.push({$matchTopN: {filter: {val: {$gte: 0}}, sort: {val: 1}, limit: 2}});
        }
        const viewName = makeView(
            jsTestName() + "_long_desugar_view",
            foreignCollName,
            viewPipeline,
        );

        const results = localColl
            .aggregate([{$lookup: {from: viewName, pipeline: [], as: "joined"}}])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, "every $matchTopN admits both docs", {results});
    });

    it("$lookup succeeds with a long desugaring user subpipeline", function () {
        // Same expansion pressure, but in the user subpipeline rather than the view definition.
        const kNumStages = 25;
        const subPipeline = [];
        for (let i = 0; i < kNumStages; i++) {
            subPipeline.push({$matchTopN: {filter: {val: {$gte: 0}}, sort: {val: 1}, limit: 2}});
        }
        const viewName = makeView(jsTestName() + "_plain_view", foreignCollName, [
            {$addFields: {fromView: true}},
        ]);

        const results = localColl
            .aggregate([{$lookup: {from: viewName, pipeline: subPipeline, as: "joined"}}])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, {results});
    });

    it("$lookup succeeds on a max-depth view chain with desugaring stages at several levels", function () {
        // A chain exactly at the depth limit, with a desugaring stage every third level; desugaring
        // must not consume depth budget.
        let source = foreignCollName;
        for (let i = 0; i < kMaxViewDepth; i++) {
            const pipeline =
                i % 3 === 0
                    ? [{$matchTopN: {filter: {val: {$gte: 0}}, sort: {val: 1}, limit: 2}}]
                    : [{$addFields: {["level" + i]: true}}];
            source = makeView(jsTestName() + "_depth_" + i, source, pipeline);
        }

        const results = localColl
            .aggregate([{$lookup: {from: source, pipeline: [], as: "joined"}}])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].joined.length, 2, {results});
        assert.eq(results[0].joined[0].level1, true, "intermediate view levels applied", {results});
    });

    it("$lookup on an over-depth view chain with desugaring stages fails with ViewDepthLimitExceeded", function () {
        // One level past the limit must be rejected with the same error as a non-extension chain,
        // not an extension-specific or internal error.
        let source = foreignCollName;
        for (let i = 0; i <= kMaxViewDepth; i++) {
            const pipeline =
                i % 3 === 0
                    ? [{$matchTopN: {filter: {val: {$gte: 0}}, sort: {val: 1}, limit: 2}}]
                    : [{$addFields: {["over" + i]: true}}];
            source = makeView(jsTestName() + "_over_depth_" + i, source, pipeline);
        }

        assert.commandFailedWithCode(
            db.runCommand({
                aggregate: localCollName,
                pipeline: [{$lookup: {from: source, pipeline: [], as: "joined"}}],
                cursor: {},
            }),
            kViewDepthExceededCode,
        );
    });

    it("$graphLookup succeeds on a max-depth view chain with desugaring stages", function () {
        let source = foreignCollName;
        for (let i = 0; i < kMaxViewDepth; i++) {
            const pipeline =
                i % 3 === 0
                    ? [{$matchTopN: {filter: {val: {$gte: 0}}, sort: {val: 1}, limit: 2}}]
                    : [{$addFields: {["glevel" + i]: true}}];
            source = makeView(jsTestName() + "_gdepth_" + i, source, pipeline);
        }

        const results = localColl
            .aggregate([
                {
                    $graphLookup: {
                        from: source,
                        startWith: "x",
                        connectFromField: "name",
                        connectToField: "name",
                        as: "connections",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].connections.length, 1, "should reach the 'x' doc", {results});
        assert.eq(results[0].connections[0].name, "x", {results});
    });
});
