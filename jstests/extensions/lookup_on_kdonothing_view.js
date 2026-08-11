/**
 * Tests $lookup, $unionWith and $graphLookup against a view whose first post-desugar stage declares
 * FirstStageViewApplicationPolicy::kDoNothing.
 *
 * $lookup discards the resolved view prefix when the subpipeline's first stage is expected to apply
 * the view itself (the mongot / kDoNothing path). With an EMPTY subpipeline there is no such stage,
 * which used to crash mongod with SIGBUS and, separately, drop the view definition so the join ran
 * against the backing collection. See FOLLOWUP-desugar-view-in-lookup-crash.md for the mechanism.
 *
 * Both are covered here. The view filters to a single document, so a dropped view definition shows
 * up as extra documents rather than as a subtle field difference.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionStubParsers,
 *   featureFlagExtensionsInsideHybridSearch,
 *   requires_fcv_90,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";

const localCollName = jsTestName() + "_local";
const foreignCollName = jsTestName() + "_foreign";
const viewName = jsTestName() + "_view";

describe("$lookup and friends against a kDoNothing-policy view", function () {
    let localColl, foreignColl;

    before(function () {
        localColl = db[localCollName];
        foreignColl = db[foreignCollName];
        localColl.drop();
        foreignColl.drop();
        assert.commandWorked(localColl.insertMany([{_id: 1, key: "K"}]));
        assert.commandWorked(
            foreignColl.insertMany([
                {_id: 10, key: "K", name: "a"},
                {_id: 11, key: "K", name: "b"},
                {_id: 12, key: "K", name: "c"},
            ]),
        );
    });

    afterEach(function () {
        assertDropCollection(db, viewName);
    });

    /**
     * Creates the view under test: $desugarAddViewName (which expands to a kDoNothing first stage)
     * followed by a $match keeping only "a". The direct-query assertion means a change in the
     * extension's semantics cannot quietly invalidate the cases below.
     */
    function createKDoNothingView() {
        assert.commandWorked(
            db.createView(viewName, foreignCollName, [
                {$desugarAddViewName: {}},
                {$match: {name: "a"}},
            ]),
        );
        const direct = db[viewName]
            .find()
            .toArray()
            .map((d) => d.name)
            .sort();
        assert.eq(direct, ["a"], "view should yield exactly the one matching doc", {direct});
    }

    it("$lookup with an empty subpipeline applies the view and does not crash the server", function () {
        createKDoNothingView();

        const results = localColl
            .aggregate([{$lookup: {from: viewName, pipeline: [], as: "j"}}])
            .toArray();
        assert.eq(results.length, 1, {results});

        const names = results[0].j.map((d) => d.name).sort();
        // Regression: this returned ["a", "b", "c"] when the view prefix was discarded.
        assert.eq(names, ["a"], "the view's $match must still apply through an empty subpipeline", {
            names,
        });

        // The historical failure mode killed mongod outright, so prove it is still up.
        assert.commandWorked(db.runCommand({ping: 1}), "server should survive the aggregate");
    });

    it("$lookup with localField/foreignField and an empty subpipeline applies the view", function () {
        createKDoNothingView();

        const results = localColl
            .aggregate([
                {$lookup: {from: viewName, localField: "key", foreignField: "key", as: "j"}},
            ])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].j.map((d) => d.name).sort(), ["a"], {results});
        assert.commandWorked(db.runCommand({ping: 1}), "server should survive the aggregate");
    });

    it("$lookup with a non-empty subpipeline applies the view", function () {
        createKDoNothingView();

        const results = localColl
            .aggregate([
                {$lookup: {from: viewName, pipeline: [{$addFields: {seen: true}}], as: "j"}},
            ])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].j.map((d) => d.name).sort(), ["a"], {results});
        for (const doc of results[0].j) {
            assert.eq(doc.seen, true, "user subpipeline should still run", {doc});
        }
    });

    it("$unionWith applies the view", function () {
        createKDoNothingView();

        const results = localColl
            .aggregate([
                {$match: {__never_matches__: 1}},
                {$unionWith: {coll: viewName, pipeline: []}},
            ])
            .toArray();
        assert.eq(results.map((d) => d.name).sort(), ["a"], {results});
    });

    it("$graphLookup applies the view", function () {
        createKDoNothingView();

        const results = localColl
            .aggregate([
                {
                    $graphLookup: {
                        from: viewName,
                        startWith: "$key",
                        connectFromField: "__none__",
                        connectToField: "key",
                        as: "j",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 1, {results});
        assert.eq(results[0].j.map((d) => d.name).sort(), ["a"], {results});
    });
});
