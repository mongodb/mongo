/**
 * Tests that the 'enableDetailedJoinExplain' knob controls whether explain includes detailed join
 * estimation fields, when set via setParameter or query settings.
 *
 * @tags: [
 *   requires_sbe,
 * ]
 */
import {
    getAllPlanStages,
    getQueryPlanner,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {plannerStageIsJoinOptNode} from "jstests/libs/query/join_utils.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";

describe("enableDetailedJoinExplain", function () {
    let rst;
    let coll;
    let qsutils;
    let pipeline;

    before(function () {
        // setQuerySettings is not supported on a standalone mongod.
        rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {setParameter: {internalEnableJoinOptimization: true}},
        });
        rst.startSet();
        rst.initiate();
        const db = rst.getPrimary().getDB(jsTestName());

        coll = db.base;
        const foreign = db.foreign;
        const docs = Array.from({length: 100}, (_, i) => ({_id: i, a: i}));
        for (const c of [coll, foreign]) {
            assert.commandWorked(c.insertMany(docs));
            // Provides the path arrayness info the join optimizer requires.
            assert.commandWorked(c.createIndex({dummy: 1, a: 1}));
        }
        pipeline = [
            {$lookup: {from: foreign.getName(), localField: "a", foreignField: "a", as: "f"}},
            {$unwind: "$f"},
        ];
        qsutils = new QuerySettingsUtils(db, coll.getName());
    });

    after(function () {
        rst.stopSet();
    });

    afterEach(function () {
        setServerParameter(false);
    });

    function setServerParameter(value) {
        assert.commandWorked(
            coll.getDB().adminCommand({setParameter: 1, internalEnableDetailedJoinExplain: value}),
        );
    }

    function explain(aggOptions = {}) {
        return coll.explain().aggregate(pipeline, aggOptions);
    }

    function explainWithPersistentQuerySettings(queryKnobs) {
        const query = qsutils.makeAggregateQueryInstance({pipeline});
        return qsutils.withQuerySettings(query, {queryKnobs}, () => explain());
    }

    function assertDetailedJoinFields(explainOutput, expectDetails) {
        const joinOptimizer = getQueryPlanner(explainOutput).joinOptimizer;
        assert(joinOptimizer, "expected joinOptimizer in queryPlanner", {explainOutput});
        for (const field of ["metrics", "collectionsWithoutPageCount"]) {
            assert.eq(joinOptimizer.hasOwnProperty(field), expectDetails, `unexpected '${field}'`, {
                joinOptimizer,
            });
        }

        const joinStages = getAllPlanStages(getWinningPlanFromExplain(explainOutput)).filter(
            plannerStageIsJoinOptNode,
        );
        assert.gt(joinStages.length, 0, "expected join-optimized stages", {explainOutput});
        for (const stage of joinStages) {
            for (const field of ["joinCostComponents", "ndvEstimates"]) {
                assert.eq(stage.hasOwnProperty(field), expectDetails, `unexpected '${field}'`, {
                    stage,
                });
            }
        }
    }

    it("is disabled by default", function () {
        assertDetailedJoinFields(explain(), false);
    });

    it("can be enabled via setParameter", function () {
        setServerParameter(true);
        assertDetailedJoinFields(explain(), true);
    });

    it("can be enabled via persistent query settings", function () {
        const queryKnobs = {enableDetailedJoinExplain: true};
        assertDetailedJoinFields(explainWithPersistentQuerySettings(queryKnobs), true);
    });

    it("can be disabled via persistent query settings, overriding setParameter", function () {
        setServerParameter(true);
        const queryKnobs = {enableDetailedJoinExplain: false};
        assertDetailedJoinFields(explainWithPersistentQuerySettings(queryKnobs), false);
    });

    it("can be enabled via per-command query settings", function () {
        const querySettings = {queryKnobs: {enableDetailedJoinExplain: true}};
        assertDetailedJoinFields(explain({querySettings}), true);
    });

    it("can be disabled via per-command query settings, overriding setParameter", function () {
        setServerParameter(true);
        const querySettings = {queryKnobs: {enableDetailedJoinExplain: false}};
        assertDetailedJoinFields(explain({querySettings}), false);
    });
});
