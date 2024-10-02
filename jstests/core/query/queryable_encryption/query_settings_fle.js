/**
 * Tests that query settings do not apply to the queries carrying encryption information.
 *
 * @tags: [
 *   # setClusterParameter can only run on mongos in sharded clusters
 *   directly_against_shardsvrs_incompatible,
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   no_selinux,
 *   requires_fcv_80,
 *   simulate_atlas_proxy_incompatible,
 *   tenant_migration_incompatible,
 * ]
 */
import {EncryptedClient, kSafeContentField} from "jstests/fle2/libs/encrypted_client_util.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const buildInfo = assert.commandWorked(db.runCommand({"buildInfo": 1}));

if (!(buildInfo.modules.includes("enterprise"))) {
    jsTestLog("Skipping test as it requires the enterprise module");
    quit();
}

const dbName = db.getName();
const collName = jsTestName();
// Drop the whole database, because encrypted collection creates more than one collection.
db.dropDatabase();

const qsutils = new QuerySettingsUtils(db, collName);

let encryptedClient = new EncryptedClient(db.getMongo(), dbName);
assert.commandWorked(encryptedClient.createEncryptionCollection(collName, {
    encryptedFields: {
        "fields": [
            {"path": "firstName", "bsonType": "string", "queries": {"queryType": "equality"}},
        ]
    }
}));
const encryptedDb = encryptedClient.getDB();

// Insert one encrypted document.
assert.commandWorked(
    encryptedDb.getCollection(collName).einsert({firstName: "Frodo", lastName: "Baggins"}));
assert.soon(() => (encryptedDb.getCollection(collName).countDocuments({}) === 1));

function assertEncryptedQuerySucceeds(query) {
    const docs = assert.commandWorked(encryptedDb.erunCommand(query)).cursor.firstBatch;
    assert.eq(1, docs.length);
    assert.eq("Frodo", docs[0].firstName);
    assert(docs[0][kSafeContentField] !== undefined);
}

const queries = {
    findLastNameEq: qsutils.makeFindQueryInstance({filter: {lastName: "Baggins"}}),
    findLastNameIn: qsutils.makeFindQueryInstance({filter: {lastName: {$in: ["Baggins", "Tuck"]}}}),
    findFirstNameEq: qsutils.makeFindQueryInstance({filter: {firstName: "Frodo"}}),
    findFirstNameIn:
        qsutils.makeFindQueryInstance({filter: {firstName: {$in: ["Bilbo", "Frodo"]}}}),
    aggregateLastNameEq:
        qsutils.makeAggregateQueryInstance({pipeline: [{$match: {lastName: "Baggins"}}]}),
    aggregateLastNameIn: qsutils.makeAggregateQueryInstance(
        {pipeline: [{$match: {lastName: {$in: ["Baggins", "Tuck"]}}}]}),
    aggregateFirstNameEq:
        qsutils.makeAggregateQueryInstance({pipeline: [{$match: {firstName: "Frodo"}}]}),
    aggregateFirstNameIn: qsutils.makeAggregateQueryInstance(
        {pipeline: [{$match: {firstName: {$in: ["Bilbo", "Frodo"]}}}]})
};

// Ensure that encrypted queries ignore query settings.
(function testEncryptedQueriesIgnoreQuerySettings() {
    for (const query of Object.values(queries)) {
        const queryToRun = qsutils.withoutDollarDB(query);

        // Add 'reject' query settings to the base query without encryption.
        const queryShapeHash = qsutils.withQuerySettings(query, {reject: true}, function() {
            // Ensure the query executed over the encrypted connection is not rejected.
            assertEncryptedQuerySucceeds(queryToRun);

            // Ensure the query executed over the unencrypted connection is rejected.
            assert.commandFailedWithCode(db.runCommand(queryToRun),
                                         ErrorCodes.QueryRejectedBySettings);

            return qsutils.getQueryShapeHashFromQuerySettings(query);
        });

        // Repeat the same test while rejecting the base query by the query shape hash.
        qsutils.withQuerySettings(queryShapeHash, {reject: true}, function() {
            // Ensure the query executed over the encrypted connection is not rejected.
            assertEncryptedQuerySucceeds(queryToRun);

            // Ensure the query executed over the unencrypted connection is rejected.
            assert.commandFailedWithCode(db.runCommand(queryToRun),
                                         ErrorCodes.QueryRejectedBySettings);
        });
    }
})();

// Ensure that encrypted queries ignore query settings for its rewritten form.
(function testEncryptedQueriesIgnoreQuerySettingsOnSafeContent() {
    // The following filter expression match the shape of 'rewritten' encrypted filter expression.
    const safeContentFilter = {[kSafeContentField]: {$elemMatch: {$in: [BinData(0, "1234")]}}};

    // Test find queries.
    qsutils.withQuerySettings(
        qsutils.makeFindQueryInstance({filter: safeContentFilter}), {reject: true}, function() {
            for (const [queryType, query] of Object.entries(queries)) {
                if (queryType.startsWith("find")) {
                    assertEncryptedQuerySucceeds(qsutils.withoutDollarDB(query));
                }
            }
        });

    // Test aggregate queries.
    qsutils.withQuerySettings(
        qsutils.makeAggregateQueryInstance({pipeline: [{$match: safeContentFilter}]}),
        {reject: true},
        function() {
            for (const [queryType, query] of Object.entries(queries)) {
                if (queryType.startsWith("aggregate")) {
                    assertEncryptedQuerySucceeds(qsutils.withoutDollarDB(query));
                }
            }
        });
})();

encryptedClient = undefined;
