/**
 * Verifies that the MultipleCollectionPathArraynessChecker is registered for all CBR
 * sampling paths, killing queries when path arrayness assumptions are violated during a yield.
 *
 * @tags: [
 *   requires_fcv_90,
 *   # The execution_control_with_prioritization suite injects executionControlDeprioritizationGate
 *   # via TestData.setParameters, which MongoRunner propagates to the mongod started by this test.
 *   # The gate can throttle the internal CBR sampling query, causing ceSamplingMetadata to be
 *   # absent from explain output and the assertions below to fail.
 *   incompatible_with_execution_control_with_prioritization,
 * ]
 */
import {checkLog} from "src/mongo/shell/check_log.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {getQueryPlanner} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod({
    setParameter: {
        featureFlagPathArrayness: true,
        internalEnablePathArrayness: true,
        internalQueryExecYieldIterations: 1,
        internalQueryExecYieldPeriodMS: 0,
        featureFlagCostBasedRanker: true,
        internalQueryPlanRanker: "costBased",
        internalQueryCBRCEMode: "samplingCE",
        // TODO SERVER-117707: remove once CBR supports SBE.
        internalQueryFrameworkControl: "forceClassicEngine",
        logComponentVerbosity: tojson({query: {verbosity: 5}}),
        // Explicitly disable for variants that enable this.
        internalQuerySamplingByStrides: false,
    },
});
assert.neq(null, conn, "mongod was unable to start up");

const testDb = conn.getDB(jsTestName());

const coll = testDb.getCollection("large");
coll.drop();
assert.commandWorked(coll.createIndex({"a.b": 1}));
assert.commandWorked(coll.createIndex({"a.c": 1}));
{
    const docs = [];
    for (let i = 0; i < 500; i++) {
        docs.push({a: {b: i, c: i}});
    }
    assert.commandWorked(coll.insertMany(docs));
}

const smallColl = testDb.getCollection("small");
smallColl.drop();
assert.commandWorked(smallColl.createIndex({"a.b": 1}));
assert.commandWorked(smallColl.createIndex({"a.c": 1}));
assert.commandWorked(
    smallColl.insertMany([
        {a: {b: 0, c: 0}},
        {a: {b: 1, c: 1}},
        {a: {b: 2, c: 2}},
        {a: {b: 3, c: 3}},
        {a: {b: 4, c: 4}},
    ]),
);

const pipeline = [{$addFields: {e: "$a.b"}}, {$match: {e: {$gte: 0}, "a.c": {$gte: 0}}}];

function getSamplingTechnique(targetColl) {
    const ns = targetColl.getFullName();
    const explain = targetColl.explain().aggregate(pipeline);
    const meta = getQueryPlanner(explain)?.ceSamplingMetadata?.[ns];
    assert(meta, "expected ceSamplingMetadata entry for namespace", {ns, explain});
    return meta.sampleTechnique;
}

function runTest(targetColl, expectedTechnique) {
    assert.commandWorked(testDb.adminCommand({clearLog: "global"}));
    targetColl.getPlanCache().clear();
    assert.eq(getSamplingTechnique(targetColl), expectedTechnique, "wrong sampling technique", {
        expectedTechnique,
    });

    // Verify the post-sampling failpoint is reachable without invalidation.
    const afterSampling1 = configureFailPoint(testDb, "hangAfterCBRSamplingGenerateSample");
    const awaitShell = startParallelShell(
        funWithArgs(
            function (dbName, collName, pipeline) {
                db.getSiblingDB(dbName).getCollection(collName).aggregate(pipeline).toArray();
            },
            testDb.getName(),
            targetColl.getName(),
            pipeline,
        ),
        conn.port,
    );
    afterSampling1.wait();
    afterSampling1.off();
    awaitShell();

    // With invalidation, the query should be killed during sampling, never reaching
    // the post-sampling failpoint.
    targetColl.getPlanCache().clear();
    assert.commandWorked(testDb.adminCommand({clearLog: "global"}));
    const afterSampling2 = configureFailPoint(testDb, "hangAfterCBRSamplingGenerateSample");
    const fp = configureFailPoint(testDb, "pathArraynessYieldInvalidation");
    assert.throwsWithCode(
        () => targetColl.aggregate(pipeline).toArray(),
        ErrorCodes.QueryPlanKilled,
    );
    fp.off();
    afterSampling2.off();
    checkLog.containsJson(conn, 11010403, {});
}

assert.commandWorked(
    testDb.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: true}),
);
runTest(coll, "seqScan");
assert.commandWorked(
    testDb.adminCommand({setParameter: 1, internalQuerySamplingBySequentialScan: false}),
);

runTest(smallColl, "fullCollScan");

assert.commandWorked(
    testDb.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "random"}),
);
runTest(coll, "random");
assert.commandWorked(
    testDb.adminCommand({setParameter: 1, internalQuerySamplingCEMethod: "chunk"}),
);

runTest(coll, "chunk");

MongoRunner.stopMongod(conn);
