/**
 * Test that executes random queries that begin with a $match with a top-level $or predicate
 * in parallel with index drops and creations. It's also used as a regression test for the bug
 * fixed in SERVER-105873.
 *
 * @tags: [
 * query_intensive_pbt,
 * # This test runs commands that are not allowed with security token: setParameter.
 * not_allowed_with_signed_security_token,
 * assumes_no_implicit_collection_creation_on_get_collection,
 * uses_parallel_shell,
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Doesn't support resharding while modifying the collection.
 * assumes_balancer_off,
 * # Time series collections do not multiple wildcard indexes.
 * exclude_from_timeseries_crud_passthrough,
 * ]
 */
import {isFCVgte} from "jstests/libs/feature_compatibility_version.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {getDocModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {getIndexModel} from "jstests/libs/property_test_helpers/models/index_models.js";
import {topLevelOrAggModel} from "jstests/libs/property_test_helpers/common_models.js";
import {
    concreteQueryFromFamily,
    createIndexesForPBT,
} from "jstests/libs/property_test_helpers/property_testing_utils.js";
import {isSlowBuild} from "jstests/libs/query/aggregation_pipeline_utils.js";

// TODO SERVER-104773: Enable this test when sanitizers are on.
if (isSlowBuild(db)) {
    jsTest.log.info("Exiting early because debug is on, opt is off, or a sanitizer is enabled.");
    quit();
}

// Seed to be used when using the document, index and query generators.
// TODO SERVER-91404: Replace this with global PBT seed.
const randomSeed = 2;
const documentCount = 200;
const indexCount = 10;
const queryCount = 60;
const iterations = 5; // Count of iterations the test makes before returning success.

const is83orAbove = isFCVgte(db, "8.3");

function generateDocuments() {
    jsTest.log.info("Generating (" + documentCount + ") documents");

    const documentModel = getDocModel();
    return fc.sample(documentModel, {seed: randomSeed, numRuns: documentCount});
}

function generateIndexes() {
    jsTest.log.info("Generating (" + indexCount + ") indexes");

    const indexModel = getIndexModel({allowPartialIndexes: false});
    return fc.sample(indexModel, {seed: randomSeed, numRuns: indexCount});
}

function generateQueries() {
    jsTest.log.info("Generating (" + queryCount + ") queries");

    const aggModel = topLevelOrAggModel({is83orAbove: is83orAbove});
    const queryShapes = fc.sample(aggModel, {seed: randomSeed, numRuns: queryCount});
    // The query model creates a query shape with several possible constant values at the
    // leaves, so at this point we'll modify the shapes by picking only the first of those constants.
    return queryShapes.map((shape) => concreteQueryFromFamily(shape, 1));
}

function runDdlThread(indexes, shutdownLatch, createIndexesForPBT) {
    while (shutdownLatch.getCount() != 0) {
        jsTest.log.info("Creating indexes");
        const names = createIndexesForPBT(db.subplanning_during_drop, indexes);

        jsTest.log.info("Dropping indexes");
        for (const name of names) {
            db.subplanning_during_drop.dropIndex(name);
        }
    }
}

function runMainThread(queries) {
    // Run each query, if they are killed because an index gets dropped, that's okay. If they crash,
    // the test will fail.
    for (let i = 0; i < 4; ++i) {
        let j = 0;
        for (const q of queries) {
            try {
                jsTest.log.info("Query (" + ++j + "): " + JSON.stringify(q.pipeline));
                db.subplanning_during_drop.aggregate(q.pipeline).toArray();
            } catch (e) {
                assert.eq(e.code, ErrorCodes.QueryPlanKilled, e);
            }
        }
    }
}

function setTestParameters(internalQueryExecYieldIterations, internalQueryExecYieldPeriodMS) {
    const res = {};

    let commandRes = db.adminCommand({
        setParameter: 1,
        internalQueryExecYieldIterations: internalQueryExecYieldIterations,
    });
    res.internalQueryExecYieldIterations = assert.commandWorked(commandRes).was;

    commandRes = db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: internalQueryExecYieldPeriodMS});
    res.internalQueryExecYieldPeriodMS = assert.commandWorked(commandRes).was;

    return res;
}

function runTest() {
    const documents = generateDocuments();
    const indexes = generateIndexes();
    const queries = generateQueries();

    const coll = db.subplanning_during_drop;
    coll.drop();

    for (const document of documents) {
        delete document._id;
        assert.commandWorked(coll.insert(document));
    }

    for (let i = 0; i < iterations; i++) {
        // Start the index operations in the DDL thread.
        const shutdownLatch = new CountDownLatch(1);
        const ddlThread = new Thread(runDdlThread, indexes, shutdownLatch, createIndexesForPBT);
        ddlThread.start();

        try {
            // Start the queries in the main thread.
            runMainThread(queries);
        } finally {
            // Shutdown the DDL thread.
            shutdownLatch.countDown();
            ddlThread.join();
        }
    }
}

jsTest.log.info("Starting test.");

let originalParameterValues;
try {
    // Increase yielding frequency so we have more chance to catch concurrency bugs due to yielding.
    originalParameterValues = setTestParameters(1, 1);
    runTest();
} finally {
    setTestParameters(
        originalParameterValues.internalQueryExecYieldIterations,
        originalParameterValues.internalQueryExecYieldPeriodMS,
    );
}

jsTest.log.info("Test finished.");
