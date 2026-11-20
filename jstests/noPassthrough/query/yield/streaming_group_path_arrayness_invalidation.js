/**
 * Verifies that streaming groups are killed when a grouping key becomes multikey while the plan
 * is yielded or saved between batches.
 *
 * @tags: [requires_fcv_91, requires_sbe]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";

const keyShapes = [
    {name: "scalar", index: {a: 1}, key: "$a", arrayDoc: {a: [100, 101]}},
    {
        name: "compound",
        index: {a: 1, b: 1},
        key: {a: "$a", b: "$b"},
        arrayDoc: {a: 100, b: [100, 101]},
    },
    {
        name: "dotted",
        index: {"x.y": 1},
        key: "$x.y",
        arrayDoc: {x: [{y: 100}, {y: 101}]},
    },
];

describe("streaming group path arrayness invalidation", function () {
    let conn, testDb, coll;

    function getInvalidationCount() {
        return assert.commandWorked(testDb.adminCommand({serverStatus: 1})).metrics.query
            .pathArrayness.queriesFailedDueToInvalidation;
    }

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagSbeStreamingGroup: true,
                featureFlagPathArrayness: true,
                internalEnablePathArrayness: true,
                internalQueryFrameworkControl: "trySbeRestricted",
                internalQueryExecYieldPeriodMS: 100000000,
                internalQueryExecYieldIterations: 100000000,
            },
        });
        assert.neq(null, conn, "mongod was unable to start up");
        testDb = conn.getDB(jsTestName());
        coll = testDb.coll;
    });

    afterEach(function () {
        assert.commandWorked(
            testDb.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 100000000}),
        );
        coll.drop();
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    function invalidateDuringYield(command, arrayDoc) {
        assert.commandWorked(
            testDb.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}),
        );
        const fp = configureFailPoint(testDb, "setYieldAllLocksHang", {
            namespace: coll.getFullName(),
        });
        let awaitShell;
        try {
            awaitShell = startParallelShell(
                funWithArgs(
                    function (dbName, command) {
                        assert.commandFailedWithCode(
                            db.getSiblingDB(dbName).runCommand(command),
                            ErrorCodes.QueryPlanKilled,
                        );
                    },
                    testDb.getName(),
                    command,
                ),
                conn.port,
            );
            fp.wait();
            assert.commandWorked(coll.insertOne(arrayDoc));
        } finally {
            fp.off();
            if (awaitShell) {
                awaitShell();
            }
        }
    }

    function invalidateBeforeGetMore(command, arrayDoc) {
        const {cursor} = assert.commandWorked(
            testDb.runCommand({...command, cursor: {batchSize: 1}}),
        );
        assert.neq(0, cursor.id, "expected an open streaming cursor", {cursor});
        try {
            assert.commandWorked(coll.insertOne(arrayDoc));
            assert.commandFailedWithCode(
                testDb.runCommand({getMore: cursor.id, collection: coll.getName(), batchSize: 1}),
                ErrorCodes.QueryPlanKilled,
            );
        } finally {
            assert.commandWorked(
                testDb.runCommand({killCursors: coll.getName(), cursors: [cursor.id]}),
            );
        }
    }

    function runTest(shape, fetch, invalidate) {
        assert.commandWorked(coll.createIndex(shape.index));
        assert.commandWorked(
            coll.insertMany(
                Array.from({length: 10}, (_, i) => ({a: i, b: i, x: {y: i}, value: i})),
            ),
        );
        const pipeline = [
            {$sort: shape.index},
            {$group: {_id: shape.key, total: {$sum: fetch ? "$value" : 1}}},
        ];
        const options = {hint: shape.index, allowDiskUse: false};
        const explain = coll.explain().aggregate(pipeline, options);
        assert(
            /\bstreaming_group\s*\[/.test(getWinningPlanFromExplain(explain, true).stages),
            "expected a lowered streaming group",
            {explain},
        );
        const plan = getWinningPlanFromExplain(explain);
        assert.eq(fetch, getPlanStage(plan, "FETCH") !== null, "unexpected access path", {plan});

        const before = getInvalidationCount();
        invalidate(
            {aggregate: coll.getName(), pipeline, ...options, cursor: {}},
            {...shape.arrayDoc, value: 1},
        );
        assert.eq(getInvalidationCount(), before + 1, "expected one path arrayness kill");

        const expected = coll.aggregate(pipeline, {...options, hint: {$natural: 1}}).toArray();
        assert.eq(11, expected.length);
        assert.sameMembers(coll.aggregate(pipeline, options).toArray(), expected);
    }

    for (const shape of keyShapes) {
        for (const fetch of [false, true]) {
            const name = `${fetch ? "fetch" : "covered"} plan with a ${shape.name} key`;
            it(`kills a ${name} during yield`, () => runTest(shape, fetch, invalidateDuringYield));
            it(`kills a ${name} before getMore`, () =>
                runTest(shape, fetch, invalidateBeforeGetMore));
        }
    }
});
