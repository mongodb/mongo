/**
 * Integration tests for $_internalDocumentResultsAndMetadata view handling.
 *
 * @tags: [
 *  featureFlagExtensionsAPI,
 *  requires_fcv_90,
 * ]
 */
import {getAggPlanStages} from "jstests/libs/query/analyze_plan.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("$_internalDocumentResultsAndMetadata view handling", function () {
    let coll, simpleView, outerView, idrmView;
    const idrmCollStats = {
        $expandToDrm: {source: {$collStats: {latencyStats: {}}}},
    };
    const idrmDisallowViews = {
        $expandToDrm: {source: {$disallowViewsSource: {}}},
    };

    before(function () {
        coll = db[jsTestName()];
        coll.drop();
        assert.commandWorked(
            coll.insertMany([
                {_id: 1, status: "active"},
                {_id: 2, status: "inactive"},
                {_id: 3, status: "active"},
            ]),
        );

        const simpleViewName = jsTestName() + "_simple";
        const innerViewName = jsTestName() + "_inner";
        const outerViewName = jsTestName() + "_outer";
        const idrmViewName = jsTestName() + "_idrm";

        assert.commandWorked(
            db.createView(simpleViewName, coll.getName(), [{$match: {status: "active"}}]),
        );
        assert.commandWorked(
            db.createView(innerViewName, coll.getName(), [{$match: {status: "active"}}]),
        );
        assert.commandWorked(
            db.createView(outerViewName, innerViewName, [{$addFields: {fromOuter: true}}]),
        );
        assert.commandWorked(
            db.createView(idrmViewName, coll.getName(), [
                {$expandToDrm: {source: {$collStats: {latencyStats: {}}}}},
            ]),
        );

        simpleView = db[simpleViewName];
        outerView = db[outerViewName];
        idrmView = db[idrmViewName];
    });

    describe("$disallowViewsSource error propagation", function () {
        it("propagates bindViewInfo error through $_idrm on a simple view", function () {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: simpleView.getName(),
                    pipeline: [idrmDisallowViews],
                    cursor: {},
                }),
                12756000,
            );
        });

        it("propagates bindViewInfo error through $_idrm on a nested view", function () {
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: outerView.getName(),
                    pipeline: [
                        {
                            $expandToDrm: {
                                source: {$disallowViewsSource: {}},
                            },
                        },
                    ],
                    cursor: {},
                }),
                12756000,
            );
        });
    });

    describe("kDefaultPrepend vs kDoNothing view pipeline merging", function () {
        it("rejects $_idrm non-first in pipeline when view definition already has $_idrm", function () {
            // $_idrm in the view definition + kDefaultPrepend first stage in user pipeline means
            // view pipeline is prepended, user's later $_idrm is no longer first; throws 40602.
            assert.commandFailedWithCode(
                db.runCommand({
                    aggregate: idrmView.getName(),
                    pipeline: [{$match: {}}, idrmCollStats],
                    cursor: {},
                }),
                40602,
            );
        });

        it("allows $_idrm first in user pipeline against an $_idrm view (kDoNothing)", function () {
            assert.commandWorked(idrmView.explain().aggregate([idrmCollStats]));
        });

        it("does not prepend view's $match in front of $_idrm on simple view", function () {
            const explain = assert.commandWorked(simpleView.explain().aggregate([idrmCollStats]));
            assert.gt(
                getAggPlanStages(explain, "$_internalDocumentResultsAndMetadata").length,
                0,
                explain,
            );
            assert.eq(getAggPlanStages(explain, "$match").length, 0, explain);
        });

        it("does not prepend any view stages in front of $_idrm on nested view", function () {
            const explain = assert.commandWorked(outerView.explain().aggregate([idrmCollStats]));
            assert.gt(
                getAggPlanStages(explain, "$_internalDocumentResultsAndMetadata").length,
                0,
                explain,
            );
            assert.eq(getAggPlanStages(explain, "$match").length, 0, explain);
            assert.eq(getAggPlanStages(explain, "$addFields").length, 0, explain);
        });
    });

    describe("kDoNothing in sub-pipelines", function () {
        it("applies inside a $unionWith sub-pipeline targeting a view", function () {
            assert.commandWorked(
                coll
                    .explain()
                    .aggregate([
                        {$unionWith: {coll: simpleView.getName(), pipeline: [idrmCollStats]}},
                    ]),
            );
        });

        it("applies inside a $lookup sub-pipeline targeting a view", function () {
            assert.commandWorked(
                coll.explain().aggregate([
                    {$limit: 1},
                    {
                        $lookup: {
                            from: simpleView.getName(),
                            pipeline: [idrmCollStats],
                            as: "joined",
                        },
                    },
                ]),
            );
        });

        it("outer $_idrm on view + inner $unionWith both honor kDoNothing without 40602", function () {
            assert.commandWorked(
                simpleView
                    .explain()
                    .aggregate([
                        idrmCollStats,
                        {$unionWith: {coll: simpleView.getName(), pipeline: [idrmCollStats]}},
                    ]),
            );
        });

        it("handles deeply nested $unionWith with $_idrm at each level", function () {
            assert.commandWorked(
                coll.explain().aggregate([
                    {
                        $unionWith: {
                            coll: simpleView.getName(),
                            pipeline: [
                                idrmCollStats,
                                {
                                    $unionWith: {
                                        coll: simpleView.getName(),
                                        pipeline: [idrmCollStats],
                                    },
                                },
                            ],
                        },
                    },
                ]),
            );
        });
    });
});
