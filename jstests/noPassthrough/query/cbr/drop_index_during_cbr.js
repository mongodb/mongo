/**
 * Dropping an index during a yield that happens during the CBR sampling query must produce
 * QueryPlanKilled.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getEngine, getRejectedPlans} from "jstests/libs/query/analyze_plan.js";
import {isPlanCosted, setCBRConfig} from "jstests/libs/query/cbr_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkSbeCompletelyDisabled} from "jstests/libs/query/sbe_util.js";

// Tests that various configurations result in QueryPlanKilled if an index is dropped.
{
    const conn = MongoRunner.runMongod({
        setParameter: {
            internalQueryFrameworkControl: "forceClassicEngine",
            internalQuerySamplingBySequentialScan: true,
            // Yield after every document so we reliably hit a yield window in the sampling query.
            internalQueryExecYieldIterations: 1,
            internalQueryExecYieldPeriodMS: 0,
        },
    });
    const db = conn.getDB("admin");

    const testDB = conn.getDB(jsTestName());
    const coll = testDB[jsTestName()];
    const kNumDocs = 20000;
    coll.drop();
    const docs = [];
    for (let i = 0; i < kNumDocs; i++) {
        docs.push({a: i, b: kNumDocs - i});
    }
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

    function runTestHelper(ceMode, autoStrategy, findFilter) {
        jsTest.log.info(
            `Running drop-index-during-CBR test: ceMode=${ceMode}, autoStrategy=${autoStrategy}, filter=${tojson(findFilter)}`,
        );

        const cbrConfig = {internalQueryCBRCEMode: ceMode};
        if (autoStrategy !== null) {
            cbrConfig.automaticCEPlanRankingStrategy = autoStrategy;
        }
        setCBRConfig(db, cbrConfig);

        // Pause just before generateSample() so we can set setYieldAllLocksHang after any multiplanning
        // trial phase has already completed. This ensures the subsequent setYieldAllLocksHang fires
        // inside the sampling executor.
        const fpBeforeSampling = configureFailPoint(db, "hangBeforeCBRSamplingGenerateSample");

        const awaitQuery = startParallelShell(
            `const testColl = db.getSiblingDB("${jsTestName()}")["${jsTestName()}"];
             assert.throwsWithCode(
                 () => testColl.find(${tojson(findFilter)}).toArray(),
                 ErrorCodes.QueryPlanKilled,
             );`,
            conn.port,
        );

        // Wait until we are about to enter generateSample() (trial phase, if any, is done).
        fpBeforeSampling.wait();

        // Set the yield hang now, scoped to our collection. The next yield will be inside
        // the sampling executor.
        const fpYield = configureFailPoint(db, "setYieldAllLocksHang", {namespace: coll.getFullName()});

        fpBeforeSampling.off();

        // Wait until the sampling scan yields (locks released, snapshot abandoned).
        fpYield.wait();

        // Drop {a:1} while locks are released.
        assert.commandWorked(testDB.runCommand({dropIndexes: coll.getName(), index: "a_1"}));

        fpYield.off();

        awaitQuery();

        // Recreate the dropped index so the collection is ready for the next test.
        assert.commandWorked(coll.createIndex({a: 1}));
    }

    function runTest(ceMode, autoStrategy) {
        const andFilter = {a: {$gte: 14999}, b: {$gte: 14999}};
        // Implicit AND: non-subplanning path (multiplanner then CBR sampling via cbr_plan_ranking.cpp).
        runTestHelper(ceMode, autoStrategy, andFilter);

        // Rooted $or: subplanning path. kSamplingCE calls generateSample() directly from
        // SubplanStage::pickBestPlan. kAutomaticCE delegates branch selection to the multiplanner
        // instead, so generateSample() is not reached on this path and this test does not apply.
        if (ceMode === "samplingCE") {
            runTestHelper(ceMode, autoStrategy, {$or: [{a: {$gte: 14999}}, {b: {$gte: 14999}}]});
        }
    }

    runTest("samplingCE", null);
    runTest("automaticCE", "CBRForNoMultiplanningResults");
    runTest("automaticCE", "CBRCostBasedRankerChoice");

    MongoRunner.stopMongod(conn);
}

// Tests that a CBR-planned query lowered to SBE (via featureFlagGetExecutorDeferredEngineChoice)
// is killed when an index is dropped during a CBR sampling yield.
// featureFlagGetExecutorDeferredEngineChoice is startup-only, so this needs its own mongod.
// Skip if the classic engine is forced by the test variant, since SBE cannot run in that case.
if (!checkSbeCompletelyDisabled(null)) {
    jsTest.log.info("Running CBR-plan-lowered-to-SBE drop-index test");

    const sbeConn = MongoRunner.runMongod({
        setParameter: {
            featureFlagGetExecutorDeferredEngineChoice: true,
            internalQuerySamplingBySequentialScan: true,
            // Yield after every document so we reliably hit a yield window in the sampling query.
            internalQueryExecYieldIterations: 1,
            internalQueryExecYieldPeriodMS: 0,
            internalQueryCBRCEMode: "samplingCE",
        },
    });
    const sbeAdminDB = sbeConn.getDB("admin");
    const sbeTestDB = sbeConn.getDB("sbeLoweringTest");
    const sbeColl = sbeTestDB["sbeLoweringTest"];
    sbeColl.drop();

    const kNumDocs = 10;
    const docs = [];
    for (let i = 0; i < kNumDocs; i++) {
        docs.push({a: i, b: kNumDocs - i});
    }
    assert.commandWorked(sbeColl.insertMany(docs));
    assert.commandWorked(sbeColl.createIndexes([{a: 1}, {b: 1}]));

    // A pipeline with $group forces the plan to be lowered to SBE under deferred engine
    // choice.
    const pipeline = [{$match: {a: {$gte: 9}, b: {$gte: 1}}}, {$group: {_id: "$a"}}];

    // Sanity-check: confirm the plan is CBR-costed and executed in SBE.
    const explainResult = sbeColl.explain().aggregate(pipeline);
    assert(
        getRejectedPlans(explainResult).some((p) => isPlanCosted(p)),
        "Expected a CBR-costed rejected plan in explain output",
    );
    assert.eq(
        getEngine(explainResult),
        "sbe",
        "Expected SBE engine with featureFlagGetExecutorDeferredEngineChoice enabled",
    );

    const fpBeforeSampling = configureFailPoint(sbeAdminDB, "hangBeforeCBRSamplingGenerateSample");

    const awaitQuery = startParallelShell(
        `assert.throwsWithCode(
             () => db.getSiblingDB("${sbeTestDB.getName()}")["${sbeColl.getName()}"].aggregate(${tojson(pipeline)}).toArray(),
             ErrorCodes.QueryPlanKilled,
         );`,
        sbeConn.port,
    );

    fpBeforeSampling.wait();

    const fpYield = configureFailPoint(sbeAdminDB, "setYieldAllLocksHang", {namespace: sbeColl.getFullName()});
    fpBeforeSampling.off();
    fpYield.wait();

    // Drop {a:1} while locks are released.
    assert.commandWorked(sbeTestDB.runCommand({dropIndexes: sbeColl.getName(), index: "a_1"}));

    fpYield.off();
    awaitQuery();

    MongoRunner.stopMongod(sbeConn);
} else {
    jsTest.log.info("Skipping CBR-plan-lowered-to-SBE test: classic engine is forced");
}

// Test that a snapshot-transaction query is protected from concurrent index drops.
// readConcern: "snapshot" requires a replica set, so this test starts its own ReplSetTest.
{
    jsTest.log.info("Running snapshot transaction test");

    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const primaryAdminDB = primary.getDB("admin");

    assert.commandWorked(
        primaryAdminDB.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
    );
    assert.commandWorked(primaryAdminDB.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}));
    assert.commandWorked(primaryAdminDB.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}));
    assert.commandWorked(primaryAdminDB.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 0}));
    setCBRConfig(primaryAdminDB, {internalQueryCBRCEMode: "samplingCE"});

    const snapshotDB = primary.getDB("snapshotCBRTest");
    const snapshotColl = snapshotDB["snapshotCBRTest"];
    snapshotColl.drop();
    const kSnapshotDocs = 10;
    const snapshotDocs = [];
    for (let i = 0; i < kSnapshotDocs; i++) {
        snapshotDocs.push({a: i, b: kSnapshotDocs - i});
    }
    assert.commandWorked(snapshotColl.insertMany(snapshotDocs));
    assert.commandWorked(snapshotColl.createIndexes([{a: 1}, {b: 1}]));

    const fpBeforeSampling = configureFailPoint(primary, "hangBeforeCBRSamplingGenerateSample");

    // Start a find inside a snapshot transaction.
    const awaitQuery = startParallelShell(() => {
        const session = db.getMongo().startSession();
        const sessionDB = session.getDatabase("snapshotCBRTest");
        session.startTransaction({readConcern: {level: "snapshot"}});
        const result = sessionDB.snapshotCBRTest.find({a: {$gte: 9}, b: {$gte: 1}}).toArray();
        assert.gt(result.length, 0, "snapshot query should return results");
        session.commitTransaction();
        session.endSession();
    }, primary.port);

    fpBeforeSampling.wait();

    // Use hangAfterAbortingIndexes to confirm the drop is in-flight before we let the
    // query proceed.
    const fpDropReady = configureFailPoint(primary, "hangAfterAbortingIndexes");

    // Start a concurrent dropIndexes.
    const awaitDrop = startParallelShell(() => {
        assert.commandWorked(
            db.getSiblingDB("snapshotCBRTest").runCommand({dropIndexes: "snapshotCBRTest", index: "a_1"}),
        );
    }, primary.port);

    // Wait until the drop command is in-flight and paused at the failpoint.
    fpDropReady.wait();

    // Resume sampling. The query runs to completion without yields, and the transaction
    // commits (releasing locks).
    fpBeforeSampling.off();
    awaitQuery();

    // Now release the drop — the transaction has committed so the X lock is available.
    fpDropReady.off();
    awaitDrop();

    rst.stopSet();
}
