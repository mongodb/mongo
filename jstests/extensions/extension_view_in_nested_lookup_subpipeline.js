/**
 * Tests view resolution when a $lookup targets a view and its subpipeline is rebuilt at execution
 * time. Two behaviors are covered:
 *   1. An extension stage ($desugarAddViewName -> [$addViewName, $doNothingViewPolicy], whose first
 *      stage has FirstStageViewApplicationPolicy::kDoNothing) run against a view that is reached
 *      indirectly — nested inside a $unionWith within the view definition that the $lookup targets —
 *      still receives its view binding and stamps the resolved view's name onto its output.
 *   2. A $lookup that targets a view whose pipeline ends in [$match, $count] applies that view
 *      exactly once, not twice.
 *
 * Uses only toy extensions, so it runs in the extensions_* passthrough suites.
 *
 * @tags: [
 *   featureFlagExtensionsAPI,
 *   featureFlagExtensionStubParsers,
 *   requires_fcv_90,
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";

const localCollName = jsTestName() + "_local";
const foreignCollName = jsTestName() + "_foreign";

describe("view targeted by $lookup with nested subpipelines", function () {
    let localColl, foreignColl;
    let createdViews;

    before(function () {
        localColl = db[localCollName];
        foreignColl = db[foreignCollName];
        localColl.drop();
        foreignColl.drop();
        assert.commandWorked(
            localColl.insertMany([
                {_id: 1, key: "A"},
                {_id: 2, key: "B"},
            ]),
        );
        assert.commandWorked(
            foreignColl.insertMany([
                {_id: 10, name: "x", val: 100},
                {_id: 11, name: "y", val: 200},
            ]),
        );
    });

    beforeEach(function () {
        createdViews = [];
    });

    afterEach(function () {
        // Drop in reverse creation order so a view is removed before the view it depends on.
        for (const viewName of createdViews.reverse()) {
            assertDropCollection(db, viewName);
        }
    });

    it("binds a kDoNothing extension on a view nested inside the view a $lookup targets", function () {
        // innerView is an ordinary view. outerView's definition runs the extension against innerView
        // through a $unionWith, so the extension is not the first stage after view resolution. The
        // $lookup targets outerView, so its subpipeline is rebuilt at execution and the nested
        // innerView binding must reach $addViewName for it to stamp innerView's name.
        const innerViewName = jsTestName() + "_inner_view";
        const outerViewName = jsTestName() + "_outer_view";
        assert.commandWorked(
            db.createView(innerViewName, foreignCollName, [{$addFields: {fromInner: true}}]),
        );
        createdViews.push(innerViewName);
        assert.commandWorked(
            db.createView(outerViewName, foreignCollName, [
                {$unionWith: {coll: innerViewName, pipeline: [{$desugarAddViewName: {}}]}},
            ]),
        );
        createdViews.push(outerViewName);

        const results = localColl
            .aggregate([{$lookup: {from: outerViewName, pipeline: [], as: "joined"}}])
            .toArray();
        assert.eq(results.length, 2, "all local docs returned", {results});

        // The $unionWith side runs the extension against innerView, stamping viewName ==
        // innerViewName. The outer side (foreignColl) does not.
        const stamped = results[0].joined.filter((d) => d.viewName === innerViewName);
        assert.eq(
            stamped.length,
            2,
            "expected both nested-view docs to carry the view name stamped by the bound extension",
            {joined: results[0].joined},
        );
    });

    it("applies a [$match, $count] view exactly once when targeted by a $lookup", function () {
        // countView => {n: 1} (one 'x' doc).
        // Applied once (correct): [$match, $count, $set y=$n] => joined: [{n:1, y:1}].
        // Applied twice:          [$match, $count, $match, $count, $set] => the second $match runs
        //                         against {n:1} (no 'name' field) => 0 docs => $count emits nothing
        //                         => joined: [].
        const countViewName = jsTestName() + "_count_view";
        assert.commandWorked(
            db.createView(countViewName, foreignCollName, [{$match: {name: "x"}}, {$count: "n"}]),
        );
        createdViews.push(countViewName);

        const results = localColl
            .aggregate([
                {
                    $lookup: {
                        from: countViewName,
                        pipeline: [{$set: {y: "$n"}}],
                        as: "joined",
                    },
                },
            ])
            .toArray();
        assert.eq(results.length, 2, "all local docs returned", {results});

        for (const doc of results) {
            assert.eq(
                doc.joined.length,
                1,
                "expected exactly one joined document from the count view (applied once)",
                {doc},
            );
            assert.eq(doc.joined[0].n, 1, "count view should report n: 1", {doc});
            assert.eq(doc.joined[0].y, 1, "user-pipeline $set y=$n should produce y: 1", {doc});
        }
    });
});
