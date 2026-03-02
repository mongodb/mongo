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
 * # Incompatible with setParameter
 * does_not_support_stepdowns,
 * # Runs queries that may return many results, requiring getmores
 * requires_getmore,
 * # Doesn't support resharding while modifying the collection.
 * assumes_balancer_off,
 * ]
 */
import {Thread} from "jstests/libs/parallelTester.js";
import {fc} from "jstests/third_party/fast_check/fc-3.1.0.js";
import {getDocModel} from "jstests/libs/property_test_helpers/models/document_models.js";
import {getIndexModel} from "jstests/libs/property_test_helpers/models/index_models.js";
import {topLevelOrAggModel} from "jstests/libs/property_test_helpers/common_models.js";
import {
    concreteQueryFromFamily,
    createIndexesForPBT,
} from "jstests/libs/property_test_helpers/property_testing_utils.js";

// Seed to be used when using the document, index and query generators.
// TODO SERVER-91404: Replace this with global PBT seed.
const randomSeed = 2;
const documentCount = 200;
const indexCount = 10;
const queryCount = 60;
const iterations = 5; // Count of iterations the test makes before returning success.

const is83orAbove = (() => {
    const {version} = db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}).featureCompatibilityVersion;
    return MongoRunner.compareBinVersions(version, "8.3") >= 0;
})();

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

function runMainThread() {
    // Run each query, if they are killed because an index gets dropped, that's okay. If they crash,
    // the test will fail.
    for (let i = 0; i < 4; ++i) {
        let j = 0;
        for (const q of queries) {
            try {
                jsTest.log.info("Query (" + ++j + ")");
                db.subplanning_during_drop.aggregate(q.pipeline).toArray();
            } catch (e) {
                if (e.code !== ErrorCodes.QueryPlanKilled) {
                    assert(false, e);
                }
            }
        }
    }
}

// Increase yielding frequency so we have more chance to catch concurrency bugs due to yielding.
assert.commandWorked(
    db.adminCommand({
        setParameter: 1,
        internalQueryExecYieldIterations: 1,
    }),
);

assert.commandWorked(db.adminCommand({setParameter: 1, internalQueryExecYieldPeriodMS: 1}));

const documents = generateDocuments();
const indexes = generateIndexes();
const queries = generateQueries();

for (let i = 0; i < iterations; i++) {
    const coll = db.subplanning_during_drop;
    coll.drop();

    for (const document of documents) {
        delete document._id;
        assert.commandWorked(coll.insert(document));
    }

    // Start the index operations in the DDL thread.
    let shutdownLatch = new CountDownLatch(1);
    const ddlThread = new Thread(runDdlThread, indexes, shutdownLatch, createIndexesForPBT);
    ddlThread.start();

    try {
        // Start the queries in the main thread.
        runMainThread(coll);
    } finally {
        // Shutdown the DDL thread.
        shutdownLatch.countDown();
        ddlThread.join();
    }
}
