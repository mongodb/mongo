/**
 * Dropping an index during a yield that happens during the CBR sampling query must produce
 * QueryPlanKilled.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {getEngine, getRejectedPlans, planHasStage} from "jstests/libs/query/analyze_plan.js";
import {isPlanCosted, setPlanRankerConfig} from "jstests/libs/query/cbr_utils.js";
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
        // 'loc2d', 'loc2dsphere' and 'txt' exist for the geo/text cases below. 2d wants a
        // legacy coordinate pair; 2dsphere gets a GeoJSON Point so the index and the
        // $near/$geometry query agree on the representation.
        docs.push({
            a: i,
            b: kNumDocs - i,
            loc2d: [i % 90, i % 45],
            loc2dsphere: {type: "Point", coordinates: [i % 90, i % 45]},
            txt: "cat dog " + i,
        });
    }
    assert.commandWorked(coll.insertMany(docs));
    assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

    // 'queryExpr' is a string of JavaScript that is spliced into the parallel shell's source
    // below. It must reference the collection as 'testColl', which the parallel shell defines,
    // e.g. "testColl.find({a: 1}).toArray()".
    // 'indexes' are dropped during the sampling yield and recreated afterwards; every candidate
    // plan's index must be listed, or the winning plan survives the drop and the query is not
    // killed.
    // 'planRanker' and 'mixedPlanRankerStrategy' (if populated) define the CBR configuration
    // under which we will run the query.
    function runTestHelper(
        planRanker,
        mixedPlanRankerStrategy,
        queryExpr,
        indexes = [{name: "a_1", spec: {a: 1}}],
    ) {
        jsTest.log.info(
            `Running drop-index-during-CBR test: planRanker=${planRanker}, mixedPlanRankerStrategy=${mixedPlanRankerStrategy}, query=${queryExpr}`,
        );

        const cbrConfig = {
            internalQueryPlanRanker: planRanker,
            internalQueryCBRCEMode: "samplingCE",
        };

        if (mixedPlanRankerStrategy !== null) {
            cbrConfig.internalQueryMixedPlanRankingStrategy = mixedPlanRankerStrategy;
        }
        setPlanRankerConfig(db, cbrConfig);

        // Pause just before generateSample() so we can set setYieldAllLocksHang after any multiplanning
        // trial phase has already completed. This ensures the subsequent setYieldAllLocksHang fires
        // inside the sampling executor.
        const fpBeforeSampling = configureFailPoint(db, "hangBeforeCBRSamplingGenerateSample");

        const awaitQuery = startParallelShell(
            `const testColl = db.getSiblingDB("${jsTestName()}")["${jsTestName()}"];
             assert.throwsWithCode(() => ${queryExpr}, ErrorCodes.QueryPlanKilled);`,
            conn.port,
        );

        // Wait until we are about to enter generateSample() (trial phase, if any, is done).
        fpBeforeSampling.wait();

        // Set the yield hang now, scoped to our collection. The next yield will be inside
        // the sampling executor.
        const fpYield = configureFailPoint(db, "setYieldAllLocksHang", {
            namespace: coll.getFullName(),
        });

        fpBeforeSampling.off();

        // Wait until the sampling scan yields (locks released, snapshot abandoned).
        fpYield.wait();

        // Drop the candidate indexes while locks are released.
        assert.commandWorked(
            testDB.runCommand({dropIndexes: coll.getName(), index: indexes.map((idx) => idx.name)}),
        );

        fpYield.off();

        awaitQuery();

        // Recreate the dropped indexes so the collection is ready for the next test.
        assert.commandWorked(coll.createIndexes(indexes.map((idx) => idx.spec)));
    }

    function runTest(planRanker, autoStrategy) {
        const andFilter = {a: {$gte: 14999}, b: {$gte: 14999}};
        // Implicit AND: non-subplanning path (multiplanner then CBR sampling via cbr_plan_ranking.cpp).
        runTestHelper(planRanker, autoStrategy, `testColl.find(${tojson(andFilter)}).toArray()`);

        // Rooted $or: subplanning path. kSamplingCE calls generateSample() directly from
        // SubplanStage::pickBestPlan. The mixed plan ranker delegates branch selection to the
        // multiplanner instead, so generateSample() is not reached on this path and this test does
        // not apply.
        if (planRanker === "costBased") {
            const orFilter = {$or: [{a: {$gte: 14999}}, {b: {$gte: 14999}}]};
            runTestHelper(planRanker, autoStrategy, `testColl.find(${tojson(orFilter)}).toArray()`);
        }
    }

    // This runs under "costBased" only, because under the "mixed" ranker MP finds results and so CBR is never invoked.
    function runDistinctScanTest() {
        const distinctIndexes = [
            {name: "a_1", spec: {a: 1}},
            {name: "a_1_b_1", spec: {a: 1, b: 1}},
        ];
        assert.commandWorked(coll.createIndex({a: 1, b: 1}));
        const distinctFilter = {a: {$gte: 14999}};
        assert(
            planHasStage(testDB, coll.explain().distinct("a", distinctFilter), "DISTINCT_SCAN"),
            "expected a DISTINCT_SCAN plan",
        );
        runTestHelper(
            "costBased",
            null,
            `testColl.distinct("a", ${tojson(distinctFilter)})`,
            distinctIndexes,
        );
        assert.commandWorked(coll.dropIndex({a: 1, b: 1}));
    }

    // These queries produce a single solution, so only explain triggers CBR costing of the plan.
    function runExplainOnlyTest(indexName, indexSpec, filter, expectedStage) {
        assert.commandWorked(coll.createIndex(indexSpec));
        assert(
            planHasStage(testDB, coll.find(filter).explain(), expectedStage),
            `expected a ${expectedStage} plan`,
        );
        runTestHelper("costBased", null, `testColl.find(${tojson(filter)}).explain()`, [
            {name: indexName, spec: indexSpec},
        ]);
        assert.commandWorked(coll.dropIndex(indexName));
    }

    runTest("costBased", null);
    runTest("mixed", "NoMultiplanningResults");
    runTest("mixed", "EstimateRankingEffort");
    runDistinctScanTest();

    runExplainOnlyTest("loc2d_2d", {loc2d: "2d"}, {loc2d: {$near: [0, 0]}}, "GEO_NEAR_2D");
    runExplainOnlyTest(
        "loc2dsphere_2dsphere",
        {loc2dsphere: "2dsphere"},
        {loc2dsphere: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}}}},
        "GEO_NEAR_2DSPHERE",
    );
    runExplainOnlyTest("txt_text", {txt: "text"}, {$text: {$search: "cat"}}, "TEXT_MATCH");

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
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "costBased",
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

    const fpYield = configureFailPoint(sbeAdminDB, "setYieldAllLocksHang", {
        namespace: sbeColl.getFullName(),
    });
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
        primaryAdminDB.adminCommand({
            setParameter: 1,
            internalQueryFrameworkControl: "forceClassicEngine",
        }),
    );
    assert.commandWorked(
        primaryAdminDB.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
    );
    assert.commandWorked(
        primaryAdminDB.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1}),
    );
    assert.commandWorked(
        primaryAdminDB.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 0}),
    );
    setPlanRankerConfig(primaryAdminDB, {
        internalQueryPlanRanker: "costBased",
        internalQueryCBRCEMode: "samplingCE",
    });

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
            db
                .getSiblingDB("snapshotCBRTest")
                .runCommand({dropIndexes: "snapshotCBRTest", index: "a_1"}),
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
