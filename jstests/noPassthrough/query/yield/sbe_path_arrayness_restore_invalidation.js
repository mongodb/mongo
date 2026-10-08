/**
 * Verifies that an SBE plan is killed when a path it assumed to be non-array becomes multikey
 * between batches, not only during a yield within a batch. 'getMore' and the $cursor stage's
 * 'loadBatch()' restore the plan through 'PlanExecutorSBE::restoreState()' rather than through the
 * yield policy.
 *
 * Time-based yielding is disabled so that the plan does not yield right after it is restored,
 * which would run the in-batch yield check and hide a missing check on restore.
 *
 * @tags: [
 *   requires_fcv_90,
 *   requires_sbe,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getAggPlanStages, getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeFullyEnabled} from "jstests/libs/query/sbe_util.js";

const kNumDocs = 100;
const kKilledMsg = "non-array path became multikey while the query was suspended";
// The array document sorts after every existing document in natural order, so the scan reaches
// it only after the restore under test.
const kArrayDoc = {_id: kNumDocs, a: [5]};
// The $lookup makes the $match run in SBE under trySbeRestricted.
const kLookupPipeline = [
    {$match: {a: 5}},
    {$lookup: {from: "foreign", localField: "_id", foreignField: "_id", as: "m"}},
];
// SBE does not absorb $redact, so the pipeline keeps a $cursor stage over the SBE plan instead of
// being optimized away into a bare SBE executor.
const kCursorStagePipeline = kLookupPipeline.concat([{$redact: "$$KEEP"}]);

describe("SBE path arrayness check on restore", function () {
    let conn, testDb, coll;

    function getInvalidationCount() {
        return assert.commandWorked(testDb.adminCommand({serverStatus: 1})).metrics.query
            .pathArrayness.queriesFailedDueToInvalidation;
    }

    // Drains the cursor with getMores of size 1, asserting the plan is killed by the arrayness
    // check rather than returning results that omit 'kArrayDoc'.
    function assertGetMoreKilled(cursorId) {
        const err = assert.throws(() => {
            let id = cursorId;
            while (id != 0) {
                const res = assert.commandWorked(
                    testDb.runCommand({getMore: id, collection: coll.getName(), batchSize: 1}),
                );
                id = res.cursor.id;
            }
        });
        assert.eq(err.code, ErrorCodes.QueryPlanKilled, "expected QueryPlanKilled", {err});
        assert(err.message.includes(kKilledMsg), "expected path arrayness kill", {err});
    }

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagPathArrayness: true,
                internalEnablePathArrayness: true,
                internalQueryExecYieldPeriodMS: 100000000,
            },
        });
        assert.neq(null, conn, "mongod was unable to start up");
        testDb = conn.getDB(jsTestName());
        coll = testDb.coll;
        assert.commandWorked(testDb.foreign.insert({_id: 0}));
    });

    beforeEach(function () {
        coll.drop();
        // A non-multikey index on 'a' lets the plan treat 'a' as non-array.
        assert.commandWorked(coll.createIndex({a: 1}));
        const docs = [];
        for (let i = 0; i < kNumDocs; i++) {
            docs.push({_id: i, a: 5});
        }
        assert.commandWorked(coll.insertMany(docs));
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("kills a find when a path becomes multikey before a getMore", function () {
        const prev = assert.commandWorked(
            testDb.adminCommand({setParameter: 1, internalQueryFrameworkControl: "trySbeEngine"}),
        ).was;
        try {
            const explain = coll.find({a: 5}).hint({$natural: 1}).explain();
            assert.eq("sbe", getEngine(explain), "expected SBE", {explain});

            const before = getInvalidationCount();
            const res = assert.commandWorked(
                testDb.runCommand({
                    find: coll.getName(),
                    filter: {a: 5},
                    hint: {$natural: 1},
                    batchSize: 1,
                }),
            );
            assert.commandWorked(coll.insert(kArrayDoc));
            assertGetMoreKilled(res.cursor.id);
            assert.eq(
                getInvalidationCount(),
                before + 1,
                "expected invalidation counter increment",
            );
        } finally {
            assert.commandWorked(
                testDb.adminCommand({setParameter: 1, internalQueryFrameworkControl: prev}),
            );
        }
    });

    it("kills an aggregate when a path becomes multikey before a getMore", function () {
        const explain = coll.explain().aggregate(kLookupPipeline, {hint: {$natural: 1}});
        assert.eq("sbe", getEngine(explain), "expected SBE", {explain});

        const before = getInvalidationCount();
        const res = assert.commandWorked(
            testDb.runCommand({
                aggregate: coll.getName(),
                pipeline: kLookupPipeline,
                hint: {$natural: 1},
                cursor: {batchSize: 1},
            }),
        );
        assert.commandWorked(coll.insert(kArrayDoc));
        assertGetMoreKilled(res.cursor.id);
        assert.eq(getInvalidationCount(), before + 1, "expected invalidation counter increment");
    });

    it("kills an aggregate when a path becomes multikey between $cursor batches", function () {
        if (checkSbeFullyEnabled(testDb)) {
            // The whole pipeline lowers to SBE, so there is no $cursor stage to hang in.
            jsTest.log.info("Skipping: SBE full bypasses the $cursor stage");
            return;
        }
        const explain = coll.explain().aggregate(kCursorStagePipeline, {hint: {$natural: 1}});
        assert.eq("sbe", getEngine(explain), "expected SBE", {explain});
        assert.eq(1, getAggPlanStages(explain, "$cursor").length, "expected $cursor", {explain});

        // Skip the first loadBatch() so the hang happens between the first and second batches.
        const fp = configureFailPoint(
            testDb,
            "hangBeforeDocumentSourceCursorLoadBatch",
            {nss: coll.getFullName()},
            {skip: 1},
        );
        const before = getInvalidationCount();
        const awaitShell = startParallelShell(
            funWithArgs(
                function (dbName, collName, pipeline, kKilledMsg) {
                    // No getMore: the command's first batch spans several $cursor batches.
                    const res = db.getSiblingDB(dbName).runCommand({
                        aggregate: collName,
                        pipeline,
                        hint: {$natural: 1},
                        cursor: {},
                    });
                    assert.commandFailedWithCode(res, ErrorCodes.QueryPlanKilled);
                    assert(res.errmsg.includes(kKilledMsg), "expected path arrayness kill", {res});
                },
                testDb.getName(),
                coll.getName(),
                kCursorStagePipeline,
                kKilledMsg,
            ),
            conn.port,
        );
        try {
            fp.wait();
            assert.commandWorked(coll.insert(kArrayDoc));
        } finally {
            // Release the parallel shell even if the steps above fail, so the failure is reported
            // instead of the shell hanging until the test times out.
            fp.off();
        }
        awaitShell();
        assert.eq(getInvalidationCount(), before + 1, "expected invalidation counter increment");
    });
});
