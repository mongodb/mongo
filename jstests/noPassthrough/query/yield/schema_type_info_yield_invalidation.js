/**
 * Verifies that queries continue after validator-derived schema metadata changes during a yield,
 * covering SBE multi-collection restores (including $lookup) and classic RequiresCollectionStage
 * restores. CollectionSchemaChecker unit tests cover invalidation when consumed schema types change.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getAggPlanStages, getEngine} from "jstests/libs/query/analyze_plan.js";
import {checkSbeRestrictedOrFullyEnabled} from "jstests/libs/query/sbe_util.js";

const kNumDocs = 20;
const kLookupPipeline = [{$lookup: {from: "foreign", localField: "a", foreignField: "a", as: "m"}}];

describe("schema type info yield invalidation", function () {
    let conn, testDb, local, foreign, other;

    function createConstraintColl(coll) {
        coll.drop();
        assert.commandWorked(
            testDb.createCollection(coll.getName(), {
                validator: {a: {$type: "number"}},
                validationLevel: "constraint",
                validationAction: "error",
            }),
        );
        const docs = [];
        for (let i = 0; i < kNumDocs; i++) {
            docs.push({_id: i, a: i});
        }
        assert.commandWorked(coll.insertMany(docs));
    }

    function dropConstraintValidator(coll) {
        assert.commandWorked(
            testDb.runCommand({
                collMod: coll.getName(),
                validator: {},
                validationLevel: "strict",
            }),
        );
    }

    function setFrameworkControl(value) {
        assert.commandWorked(
            testDb.adminCommand({setParameter: 1, internalQueryFrameworkControl: value}),
        );
    }

    // Runs 'fn' with internal yields disabled, so a getMore restores the plan without one. Otherwise
    // the restore inside the yield runs the checks and the external restore path is never reached.
    function withoutInternalYields(fn) {
        assert.commandWorked(
            testDb.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: -1,
                internalQueryExecYieldPeriodMS: 60 * 60 * 1000,
            }),
        );
        try {
            fn();
        } finally {
            assert.commandWorked(
                testDb.adminCommand({
                    setParameter: 1,
                    internalQueryExecYieldIterations: 1,
                    internalQueryExecYieldPeriodMS: 10,
                }),
            );
        }
    }

    // Hangs 'cmdObj' on its first yield, runs 'duringYieldFn', then releases it.
    function runWithYieldHang({cmdObj, duringYieldFn}) {
        const fp = configureFailPoint(testDb, "setYieldAllLocksHang", {
            namespace: local.getFullName(),
        });
        const awaitShell = startParallelShell(
            funWithArgs(
                function (dbName, cmdObj, kNumDocs) {
                    const res = db.getSiblingDB(dbName).runCommand(cmdObj);
                    assert.commandWorked(res);
                    assert.eq(res.cursor.firstBatch.length, kNumDocs, res);
                },
                testDb.getName(),
                cmdObj,
                kNumDocs,
            ),
            conn.port,
        );
        fp.wait();
        duringYieldFn();
        fp.off();
        awaitShell();
    }

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagQueryTypeInference: true,
                internalQueryExecYieldIterations: 1,
            },
        });
        assert.neq(null, conn, "mongod was unable to start up");
        testDb = conn.getDB(jsTestName());
        local = testDb.local;
        foreign = testDb.foreign;
        other = testDb.other;
    });

    beforeEach(function () {
        createConstraintColl(local);
        createConstraintColl(foreign);
        createConstraintColl(other);
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    describe("SBE", function () {
        const lookupCmd = {aggregate: "local", pipeline: kLookupPipeline, cursor: {}};

        beforeEach(function () {
            setFrameworkControl("trySbeEngine");
        });

        it("pushes $lookup down to SBE", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            const explain = local.explain().aggregate(kLookupPipeline);
            assert.eq("sbe", getEngine(explain), explain);
            assert.eq(1, getAggPlanStages(explain, "EQ_LOOKUP").length, explain);
        });

        it("does not kill the query when an unrelated constraint validator is dropped", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            runWithYieldHang({
                cmdObj: lookupCmd,
                duringYieldFn: () => dropConstraintValidator(other),
            });
        });

        it("does not kill the query when the main collection's constraint validator is dropped", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            runWithYieldHang({
                cmdObj: lookupCmd,
                duringYieldFn: () => dropConstraintValidator(local),
            });
        });

        it("does not kill the query when the secondary collection's constraint validator is dropped", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            runWithYieldHang({
                cmdObj: lookupCmd,
                duringYieldFn: () => dropConstraintValidator(foreign),
            });
        });

        // A getMore restores the plan without going through an internal yield, so it exercises
        // PlanExecutorSBE::restoreState rather than PlanYieldPolicySBE::restoreState.
        it("does not kill a cursor on getMore when the constraint validator is dropped between batches", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            withoutInternalYields(() => {
                const res = assert.commandWorked(
                    testDb.runCommand({
                        aggregate: "local",
                        pipeline: kLookupPipeline,
                        cursor: {batchSize: 1},
                    }),
                );
                assert.neq(NumberLong(0), res.cursor.id, res);

                dropConstraintValidator(local);

                assert.commandWorked(
                    testDb.runCommand({getMore: res.cursor.id, collection: "local"}),
                );
            });
        });

        it("does not kill a cursor on getMore when an unrelated constraint validator is dropped", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            withoutInternalYields(() => {
                const res = assert.commandWorked(
                    testDb.runCommand({
                        aggregate: "local",
                        pipeline: kLookupPipeline,
                        cursor: {batchSize: 1},
                    }),
                );
                assert.neq(NumberLong(0), res.cursor.id, res);

                dropConstraintValidator(other);

                assert.commandWorked(
                    testDb.runCommand({getMore: res.cursor.id, collection: "local"}),
                );
            });
        });
    });

    describe("classic", function () {
        const findCmd = {find: "local", filter: {}, hint: {$natural: 1}, batchSize: kNumDocs + 1};

        beforeEach(function () {
            setFrameworkControl("forceClassicEngine");
        });

        it("uses the classic engine", function () {
            const explain = local.find({}).hint({$natural: 1}).explain();
            assert.eq("classic", getEngine(explain), explain);
        });

        it("does not kill the query when an unrelated constraint validator is dropped", function () {
            runWithYieldHang({
                cmdObj: findCmd,
                duringYieldFn: () => dropConstraintValidator(other),
            });
        });

        it("does not kill the query when the collection's constraint validator is dropped", function () {
            runWithYieldHang({
                cmdObj: findCmd,
                duringYieldFn: () => dropConstraintValidator(local),
            });
        });

        it("does not kill a cursor on getMore when the constraint validator is dropped between batches", function () {
            withoutInternalYields(() => {
                const res = assert.commandWorked(testDb.runCommand({...findCmd, batchSize: 1}));
                assert.neq(NumberLong(0), res.cursor.id, res);

                dropConstraintValidator(local);

                assert.commandWorked(
                    testDb.runCommand({getMore: res.cursor.id, collection: "local"}),
                );
            });
        });

        it("does not kill a cursor on getMore when an unrelated constraint validator is dropped", function () {
            withoutInternalYields(() => {
                const res = assert.commandWorked(testDb.runCommand({...findCmd, batchSize: 1}));
                assert.neq(NumberLong(0), res.cursor.id, res);

                dropConstraintValidator(other);

                assert.commandWorked(
                    testDb.runCommand({getMore: res.cursor.id, collection: "local"}),
                );
            });
        });

        it("does not kill a cursor when only validationAction changes between batches", function () {
            const res = assert.commandWorked(testDb.runCommand({...findCmd, batchSize: 1}));
            assert.neq(NumberLong(0), res.cursor.id, res);

            assert.commandWorked(
                testDb.runCommand({collMod: local.getName(), validationAction: "errorAndLog"}),
            );

            assert.commandWorked(testDb.runCommand({getMore: res.cursor.id, collection: "local"}));
        });
    });
});

describe("schema type info yield invalidation with the feature flag disabled", function () {
    let conn, testDb, coll;

    before(function () {
        conn = MongoRunner.runMongod({setParameter: {featureFlagQueryTypeInference: false}});
        assert.neq(null, conn, "mongod was unable to start up");
        testDb = conn.getDB(jsTestName());
        coll = testDb.local;
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("does not kill a cursor when the validator changes between batches", function () {
        coll.drop();
        assert.commandWorked(
            testDb.createCollection(coll.getName(), {
                validator: {a: {$type: "number"}},
                validationLevel: "constraint",
                validationAction: "error",
            }),
        );
        assert.commandWorked(
            coll.insertMany(Array.from({length: kNumDocs}, (_, i) => ({_id: i, a: i}))),
        );

        const res = assert.commandWorked(
            testDb.runCommand({
                find: coll.getName(),
                filter: {},
                hint: {$natural: 1},
                batchSize: 1,
            }),
        );
        assert.neq(NumberLong(0), res.cursor.id, res);

        assert.commandWorked(
            testDb.runCommand({
                collMod: coll.getName(),
                validator: {a: {$type: "string"}},
                validationLevel: "strict",
            }),
        );

        assert.commandWorked(
            testDb.runCommand({getMore: res.cursor.id, collection: coll.getName()}),
        );
    });
});
