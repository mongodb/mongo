/**
 * Tests executable extension stages in view contexts. Includes:
 *   - Desugar stages (e.g. $readNDocuments)
 *   - Source stages (e.g. $toast)
 *   - Transform stages (e.g. $extensionLimit)
 *
 * @tags: [featureFlagExtensionsAPI]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const testDb = db.getSiblingDB(jsTestName());
testDb.dropDatabase();

// Source extension stages run per-shard, producing N docs on each shard rather than N total.
// Tests that depend on exact document counts use relaxed assertions.
const isSharded = FixtureHelpers.isMongos(db);

const coll = testDb[jsTestName()];

const documents = [
    {_id: 0, x: 1, name: "alfa"},
    {_id: 1, x: 2, name: "bravo"},
    {_id: 2, x: 3, name: "charlie"},
    {_id: 3, x: 4, name: "delta"},
    {_id: 4, x: 5, name: "echo"},
];
assert.commandWorked(coll.insertMany(documents));

// Foreign collection for $unionWith tests.
const foreignColl = testDb[jsTestName() + "_foreign"];
const foreignDocuments = [
    {_id: 0, x: 10, name: "foxtrot"},
    {_id: 1, x: 20, name: "golf"},
    {_id: 2, x: 30, name: "hotel"},
];
assert.commandWorked(foreignColl.insertMany(foreignDocuments));

let viewCounter = 0;
function makeView(suffix, source, pipeline) {
    const name = jsTestName() + "_view_" + viewCounter++ + "_" + suffix;
    assert.commandWorked(testDb.createView(name, source, pipeline));
    return name;
}

// Desugar stage ($readNDocuments) in view contexts.
// $readNDocuments desugars to $produceIds + $_internalSearchIdLookup.
{
    // Test a desugar stage in a view definition.
    {
        const viewName = makeView("readNDocuments_in_def", coll.getName(), [{$readNDocuments: {numDocs: 3}}]);
        const results = testDb[viewName].aggregate([]).toArray();
        assertArrayEq({
            actual: results,
            expected: [
                {_id: 0, x: 1, name: "alfa"},
                {_id: 1, x: 2, name: "bravo"},
                {_id: 2, x: 3, name: "charlie"},
            ],
        });
    }

    // Verify running $readNDocuments on a view fails because the view's pipeline gets prepended,
    // making the desugared source stage no longer first.
    {
        const viewName = makeView("readNDocuments_on_view_fails", coll.getName(), [{$match: {x: {$gt: 0}}}]);
        assert.throwsWithCode(() => testDb[viewName].aggregate([{$readNDocuments: {numDocs: 2}}]).toArray(), 40602);
    }

    // Test a desugar stage in a nested view (view on view).
    {
        const baseViewName = makeView("readNDocuments_nested_base", coll.getName(), [{$readNDocuments: {numDocs: 4}}]);
        const nestedViewName = makeView("readNDocuments_nested_top", baseViewName, [
            {$addFields: {fromView: true}},
            {$match: {x: {$lte: 3}}},
        ]);

        const results = testDb[nestedViewName].aggregate([]).toArray();
        assertArrayEq({
            actual: results,
            expected: [
                {_id: 0, x: 1, name: "alfa", fromView: true},
                {_id: 1, x: 2, name: "bravo", fromView: true},
                {_id: 2, x: 3, name: "charlie", fromView: true},
            ],
        });
    }
}

// Source stage ($toast) in view contexts.
{
    // Source stage in a view definition.
    {
        const viewName = makeView("toast_in_def", coll.getName(), [{$toast: {temp: 350.0, numSlices: 3}}]);
        const results = testDb[viewName].aggregate([]).toArray();
        if (!isSharded) {
            assert.docEq(results, [
                {slice: 0, isBurnt: false},
                {slice: 1, isBurnt: false},
                {slice: 2, isBurnt: false},
            ]);
        } else {
            assert.gte(results.length, 3, results);
        }
    }

    // Verify running a source stage on a view fails because the view's pipeline gets prepended.
    {
        const viewName = makeView("toast_on_view_fails", coll.getName(), [{$match: {x: {$gt: 0}}}]);
        assert.throwsWithCode(
            () => testDb[viewName].aggregate([{$toast: {temp: 350.0, numSlices: 2}}]).toArray(),
            40602,
        );
    }

    // Test a source stage in a nested view.
    {
        const baseViewName = makeView("toast_nested_base", coll.getName(), [{$toast: {temp: 350.0, numSlices: 4}}]);
        const nestedViewName = makeView("toast_nested_top", baseViewName, [{$match: {slice: {$lte: 1}}}]);

        const results = testDb[nestedViewName].aggregate([]).toArray();
        if (!isSharded) {
            assert.docEq(results, [
                {slice: 0, isBurnt: false},
                {slice: 1, isBurnt: false},
            ]);
        } else {
            assert.gte(results.length, 2, results);
            assert(
                results.every((r) => r.slice <= 1),
                "Nested view should only return slices <= 1",
            );
        }
    }
}

// Transform stage ($extensionLimit) in view contexts.
{
    // Transform stage in a view definition.
    {
        const viewName = makeView("extensionLimit_in_def", coll.getName(), [{$sort: {_id: 1}}, {$extensionLimit: 3}]);
        const results = testDb[viewName].aggregate([]).toArray();
        assertArrayEq({
            actual: results,
            expected: [
                {_id: 0, x: 1, name: "alfa"},
                {_id: 1, x: 2, name: "bravo"},
                {_id: 2, x: 3, name: "charlie"},
            ],
        });
    }

    // Transform stage on a view.
    {
        const viewName = makeView("extensionLimit_on_view", coll.getName(), [{$sort: {_id: 1}}]);
        const results = testDb[viewName].aggregate([{$extensionLimit: 2}]).toArray();
        assertArrayEq({
            actual: results,
            expected: [
                {_id: 0, x: 1, name: "alfa"},
                {_id: 1, x: 2, name: "bravo"},
            ],
        });
    }

    // Transform stage in nested view, where the base view uses a desugar stage.
    {
        const baseViewName = makeView("extensionLimit_nested_base", coll.getName(), [{$readNDocuments: {numDocs: 4}}]);
        const nestedViewName = makeView("extensionLimit_nested_top", baseViewName, [
            {$addFields: {nested: true}},
            {$extensionLimit: 2},
        ]);

        const results = testDb[nestedViewName].aggregate([]).toArray();
        if (!isSharded) {
            assertArrayEq({
                actual: results,
                expected: [
                    {_id: 0, x: 1, name: "alfa", nested: true},
                    {_id: 1, x: 2, name: "bravo", nested: true},
                ],
            });
        } else {
            assert.eq(results.length, 2, results);
            assert(
                results.every((doc) => doc.nested),
                results,
            );
        }
    }
}

// Tests combinations of extensions-on-view with $unionWith subpipelines.
{
    // View with desugar stage + $unionWith with source stage in subpipeline.
    {
        const viewName = makeView("combo_desugar_unionWith_source", coll.getName(), [{$readNDocuments: {numDocs: 2}}]);
        const pipeline = [{$toast: {temp: 350.0, numSlices: 2}}];
        const results = testDb[viewName].aggregate([{$unionWith: {coll: coll.getName(), pipeline}}]).toArray();

        if (!isSharded) {
            assertArrayEq({
                actual: results,
                expected: [
                    {_id: 0, x: 1, name: "alfa"},
                    {_id: 1, x: 2, name: "bravo"},
                    {slice: 0, isBurnt: false},
                    {slice: 1, isBurnt: false},
                ],
            });
        } else {
            assert.gte(results.length, 4, results);
        }
    }

    // Nested view with extension stages + $unionWith on a view with extension stage.
    if (!isSharded) {
        const baseViewName = makeView("combo_nested_base", coll.getName(), [{$sort: {_id: 1}}, {$extensionLimit: 3}]);
        const nestedViewName = makeView("combo_nested_top", baseViewName, [{$addFields: {source: "nested"}}]);
        const unionViewName = makeView("combo_union_target", foreignColl.getName(), [{$readNDocuments: {numDocs: 2}}]);

        const results = testDb[nestedViewName].aggregate([{$unionWith: unionViewName}]).toArray();

        assertArrayEq({
            actual: results,
            expected: [
                {_id: 0, x: 1, name: "alfa", source: "nested"},
                {_id: 1, x: 2, name: "bravo", source: "nested"},
                {_id: 2, x: 3, name: "charlie", source: "nested"},
                {_id: 0, x: 10, name: "foxtrot"},
                {_id: 1, x: 20, name: "golf"},
            ],
        });
    }

    // $unionWith + extension stage in a view definition.
    {
        const viewPipeline = [
            {$match: {x: {$lte: 2}}},
            {$unionWith: {coll: foreignColl.getName(), pipeline: [{$sort: {_id: 1}}, {$extensionLimit: 2}]}},
        ];
        const viewName = makeView("combo_unionWith_in_def", coll.getName(), viewPipeline);

        const results = testDb[viewName].aggregate([{$match: {name: {$ne: "bravo"}}}]).toArray();
        assertArrayEq({
            actual: results,
            expected: [
                {_id: 0, x: 1, name: "alfa"},
                {_id: 0, x: 10, name: "foxtrot"},
                {_id: 1, x: 20, name: "golf"},
            ],
        });
    }
}
