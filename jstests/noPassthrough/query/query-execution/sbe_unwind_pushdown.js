import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    aggPlanHasStage,
    getEngine,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {checkSbeCompletelyDisabled} from "jstests/libs/query/sbe_util.js";

describe("$unwind pushdown to SBE", function () {
    let conn;
    let db;
    let coll;

    // True when the mongod we started cannot run SBE at all, e.g. because resmoke passed
    // 'forceClassicEngine' through TestData; every test case then early-returns.
    let sbeCompletelyDisabled = false;

    // Mochalite has no dynamic skip, so gate each test case on the runtime SBE check instead.
    const itIfSbe = (title, fn) => it(title, () => sbeCompletelyDisabled || fn());

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                internalQueryFrameworkControl: "trySbeEngine",
                featureFlagSbeUnwind: true,
            },
        });
        assert.neq(conn, null, "mongod failed to start up");

        db = conn.getDB(jsTestName());
        sbeCompletelyDisabled = checkSbeCompletelyDisabled(db);
        if (sbeCompletelyDisabled) {
            jsTest.log.info("Skipping: SBE is completely disabled");
            return;
        }

        coll = db.unwind_pushdown;
        coll.drop();
        for (let i = 0; i < 5; i++) {
            assert.commandWorked(coll.insert({_id: i, a: i % 2, arr: [i, i + 10]}));
        }
    });

    after(() => conn && MongoRunner.stopMongod(conn));

    function sbePlanHasStage(explain, stageName) {
        const sbePlan = getWinningPlanFromExplain(explain, true /* isSBEPlan */);
        return new RegExp(`\\b${stageName}\\b`).test(sbePlan.stages);
    }

    function assertGroupPushedDown(pipeline) {
        const explain = coll.explain().aggregate(pipeline);
        assert.eq(getEngine(explain), "sbe", "expected SBE engine", {pipeline, explain});
        assert(!aggPlanHasStage(explain, "$group"), "expected $group to be pushed down", {explain});
        return explain;
    }

    function assertUnwindPushedDown(pipeline) {
        const explain = assertGroupPushedDown(pipeline);
        assert(!aggPlanHasStage(explain, "$unwind"), "expected $unwind to be pushed down", {
            explain,
        });
        assert(sbePlanHasStage(explain, "unwind"), "expected unwind stage in SBE plan", {explain});
    }

    function assertTrailingUnwindPruned(pipeline) {
        const explain = assertGroupPushedDown(pipeline);
        const stages = explain.stages;
        assert(
            stages[stages.length - 1].hasOwnProperty("$unwind"),
            "expected the trailing $unwind to run in classic after the SBE plan",
            {explain},
        );
        assert(!sbePlanHasStage(explain, "unwind"), "expected no unwind stage in SBE plan", {
            explain,
        });
    }

    const groupUnwindLimit = [
        {$group: {_id: "$a", pushed: {$push: "$arr"}}},
        {$unwind: "$pushed"},
        {$limit: 10},
    ];

    itIfSbe("pushes down $unwind following $group", function () {
        assertUnwindPushedDown(groupUnwindLimit);
    });

    itIfSbe("keeps a trailing $unwind in classic", function () {
        const pipeline = [{$group: {_id: "$a", pushed: {$push: "$arr"}}}, {$unwind: "$pushed"}];
        assertTrailingUnwindPruned(pipeline);
    });

    itIfSbe("pushes down a bare $unwind with $limit", function () {
        const pipeline = [{$unwind: "$arr"}, {$limit: 10}];
        assertUnwindPushedDown(pipeline);
    });

    const setFrameworkControl = (value) =>
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: value}),
        );

    itIfSbe("keeps $unwind in classic when forceClassicEngine is set", function () {
        try {
            setFrameworkControl("forceClassicEngine");
            const explain = coll.explain().aggregate(groupUnwindLimit);
            assert.eq(getEngine(explain), "classic", "expected classic engine", {explain});
            assert(aggPlanHasStage(explain, "$unwind"), "expected $unwind in classic", {explain});
        } finally {
            setFrameworkControl("trySbeEngine");
        }
    });
});
