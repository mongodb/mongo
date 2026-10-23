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
const kKilledMsg = "collection schema metadata changed during yield";

// The $match consumes the inferred type of 'a' on the main collection, and the $lookup join fields
// consume it on the foreign collection.
const kLookupPipeline = [
    {$match: {a: {$type: "number"}}},
    {$lookup: {from: "foreign", localField: "a", foreignField: "a", as: "m"}},
];

describe("schema type info yield invalidation", function () {
    let conn, testDb, local, foreign, other;

    function createConstraintColl(coll) {
        coll.drop();
        assert.commandWorked(
            testDb.createCollection(coll.getName(), {
                // The '$not: {$type: array}' conjunct is what proves 'a' can never be an array (a
                // plain '$type' predicate still matches arrays of that type). Only then does a
                // query consuming 'a's inferred type become schema-dependent.
                validator: {$and: [{a: {$type: "number"}}, {a: {$not: {$type: "array"}}}]},
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

    // Advances the schema epoch without changing any inferred type.
    function changeValidationActionOnly(coll) {
        assert.commandWorked(
            testDb.runCommand({collMod: coll.getName(), validationAction: "errorAndLog"}),
        );
    }

    function setFrameworkControl(value) {
        assert.commandWorked(
            testDb.adminCommand({setParameter: 1, internalQueryFrameworkControl: value}),
        );
    }

    // Runs 'fn' with internal yields disabled, so a getMore restores the plan without one.
    // Otherwise the restore inside the yield runs the checks and the PlanExecutor restore path is
    // never reached.
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

    // Opens a cursor, runs 'betweenBatchesFn', then asserts the getMore outcome.
    function runWithGetMore({cmdObj, collName, betweenBatchesFn, killedNss}) {
        withoutInternalYields(() => {
            const res = assert.commandWorked(testDb.runCommand(cmdObj));
            assert.neq(NumberLong(0), res.cursor.id, res);

            betweenBatchesFn();

            const getMoreRes = testDb.runCommand({getMore: res.cursor.id, collection: collName});
            if (killedNss) {
                assert.commandFailedWithCode(getMoreRes, ErrorCodes.QueryPlanKilled);
                assert(getMoreRes.errmsg.includes(kKilledMsg), getMoreRes);
                assert(getMoreRes.errmsg.includes(killedNss), getMoreRes);
            } else {
                assert.commandWorked(getMoreRes);
            }
        });
    }

    // Hangs 'cmdObj' on its first yield, runs 'duringYieldFn', then releases it. If 'killedNss' is
    // set, asserts the command is killed because that namespace's schema metadata changed.
    function runWithYieldHang({cmdObj, duringYieldFn, killedNss, expectedCount = kNumDocs}) {
        const fp = configureFailPoint(testDb, "setYieldAllLocksHang", {
            namespace: local.getFullName(),
        });
        const awaitShell = startParallelShell(
            funWithArgs(
                function (dbName, cmdObj, killedNss, kKilledMsg, expectedCount) {
                    const res = db.getSiblingDB(dbName).runCommand(cmdObj);
                    if (killedNss) {
                        assert.commandFailedWithCode(res, ErrorCodes.QueryPlanKilled);
                        assert(res.errmsg.includes(kKilledMsg), res.errmsg);
                        assert(res.errmsg.includes(killedNss), res.errmsg);
                    } else {
                        assert.commandWorked(res);
                        assert.eq(res.cursor.firstBatch.length, expectedCount, res);
                    }
                },
                testDb.getName(),
                cmdObj,
                killedNss,
                kKilledMsg,
                expectedCount,
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
        const lookupGetMoreCmd = {
            aggregate: "local",
            pipeline: kLookupPipeline,
            cursor: {batchSize: 1},
        };

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

        it("kills the query when the main collection's constraint validator is dropped", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            runWithYieldHang({
                cmdObj: lookupCmd,
                duringYieldFn: () => dropConstraintValidator(local),
                killedNss: local.getFullName(),
            });
        });

        // SBE registers a checker for every collection the plan reads, so a change to the foreign
        // collection's schema invalidates the plan too.
        it("kills the query when the secondary collection's constraint validator is dropped", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            runWithYieldHang({
                cmdObj: lookupCmd,
                duringYieldFn: () => dropConstraintValidator(foreign),
                killedNss: foreign.getFullName(),
            });
        });

        // A getMore restores the plan without going through an internal yield, so it exercises
        // PlanExecutorSBE::restoreState rather than PlanYieldPolicySBE::restoreState.
        it("kills a cursor on getMore when the constraint validator is dropped between batches", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            runWithGetMore({
                cmdObj: lookupGetMoreCmd,
                collName: "local",
                betweenBatchesFn: () => dropConstraintValidator(local),
                killedNss: local.getFullName(),
            });
        });

        it("does not kill a cursor on getMore when an unrelated constraint validator is dropped", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            runWithGetMore({
                cmdObj: lookupGetMoreCmd,
                collName: "local",
                betweenBatchesFn: () => dropConstraintValidator(other),
            });
        });

        // Republishing identical metadata must not kill: the checker compares inferred types, not
        // a version counter.
        it("does not kill a cursor when only validationAction changes between batches", function () {
            if (!checkSbeRestrictedOrFullyEnabled(testDb)) {
                return;
            }
            runWithGetMore({
                cmdObj: lookupGetMoreCmd,
                collName: "local",
                betweenBatchesFn: () => changeValidationActionOnly(local),
            });
        });
    });

    describe("classic", function () {
        const findCmd = {find: "local", filter: {a: {$type: "number"}}, batchSize: kNumDocs + 1};
        // SBE's filter generator is currently the only production consumer of the inferred type,
        // so no classic query is marked schema-dependent and none registers a schema checker.
        // These cases pin that: classic must survive a validator change, including on its own
        // collection. Add a kill case here when a classic consumer lands.
        const aggPipeline = [{$match: {a: {$type: "number"}}}];
        const aggCmd = {aggregate: "local", pipeline: aggPipeline, cursor: {}};
        const aggGetMoreCmd = {aggregate: "local", pipeline: aggPipeline, cursor: {batchSize: 1}};

        beforeEach(function () {
            setFrameworkControl("forceClassicEngine");
        });

        it("uses the classic engine", function () {
            const explain = local.find({a: {$type: "number"}}).explain();
            assert.eq("classic", getEngine(explain), explain);
        });

        it("does not kill the query when an unrelated constraint validator is dropped", function () {
            runWithYieldHang({
                cmdObj: findCmd,
                duringYieldFn: () => dropConstraintValidator(other),
            });
        });

        // A classic find never consults the inferred type, so it is not schema-dependent and must
        // survive even a change to its own collection's validator.
        it("does not kill a find when the collection's constraint validator is dropped", function () {
            runWithYieldHang({
                cmdObj: findCmd,
                duringYieldFn: () => dropConstraintValidator(local),
            });
        });

        it("does not kill an aggregation when the collection's constraint validator is dropped", function () {
            runWithYieldHang({
                cmdObj: aggCmd,
                duringYieldFn: () => dropConstraintValidator(local),
            });
        });

        it("does not kill an aggregation when an unrelated constraint validator is dropped", function () {
            runWithYieldHang({
                cmdObj: aggCmd,
                duringYieldFn: () => dropConstraintValidator(other),
            });
        });

        it("does not kill a cursor on getMore when the constraint validator is dropped between batches", function () {
            runWithGetMore({
                cmdObj: aggGetMoreCmd,
                collName: "local",
                betweenBatchesFn: () => dropConstraintValidator(local),
            });
        });

        it("does not kill a cursor on getMore when an unrelated constraint validator is dropped", function () {
            runWithGetMore({
                cmdObj: aggGetMoreCmd,
                collName: "local",
                betweenBatchesFn: () => dropConstraintValidator(other),
            });
        });

        it("does not kill a cursor when only validationAction changes between batches", function () {
            runWithGetMore({
                cmdObj: aggGetMoreCmd,
                collName: "local",
                betweenBatchesFn: () => changeValidationActionOnly(local),
            });
        });
    });
});

describe("schema type info yield invalidation with the feature flag disabled", function () {
    let conn, testDb, coll;

    before(function () {
        conn = MongoRunner.runMongod({
            setParameter: {
                featureFlagQueryTypeInference: false,
                internalQueryExecYieldIterations: 1,
            },
        });
        assert.neq(null, conn, "mongod was unable to start up");
        testDb = conn.getDB(jsTestName());
        coll = testDb.local;
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    // With the flag off no schema metadata is registered, so even a query that would otherwise
    // consume the inferred type is never marked and must survive a validator change.
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
                aggregate: coll.getName(),
                pipeline: [{$match: {a: {$type: "number"}}}],
                cursor: {batchSize: 1},
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
