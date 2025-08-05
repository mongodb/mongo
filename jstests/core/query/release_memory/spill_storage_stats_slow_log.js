/**
 * Test that verifies that spillStorage is logged as part of slow query logging. Borrows spilling
 * testing from set_window_fields.js.
 *
 * @tags: [
 *   requires_fcv_82,
 *   # We modify the value of a query knob. setParameter is not persistent.
 *   does_not_support_stepdowns,
 *   # This test runs commands that are not allowed with security token: setParameter.
 *   not_allowed_with_signed_security_token,
 *   assumes_read_preference_unchanged,
 *   does_not_support_transactions,
 *   # releaseMemory needs special permission
 *   assumes_superuser_permissions,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

// TODO (SERVER-106716): Remove the feature flag check.
if (!FeatureFlagUtil.isPresentAndEnabled(db, "FeatureFlagCreateSpillKVEngine")) {
    quit();
}

const dbName = jsTestName();
const collName = "testcoll";
const testDB = db.getSiblingDB(dbName);
const coll = testDB[collName];

const memoryKnob = "internalDocumentSourceSetWindowFieldsMaxMemoryBytes";
function getServerParameter() {
    return assert.commandWorked(db.adminCommand({getParameter: 1, [memoryKnob]: 1}))[memoryKnob];
}
function setServerParameter(value) {
    setParameterOnAllHosts(DiscoverTopology.findNonConfigNodes(db.getMongo()), memoryKnob, value);
}
const memoryInitialValue = getServerParameter();

const docs = [
    {"_id": 1, "category": "Electronics", "amount": 100, "date": "2023-01-01"},
    {"_id": 2, "category": "Electronics", "amount": 200, "date": "2023-01-02"},
    {"_id": 3, "category": "Furniture", "amount": 150, "date": "2023-01-01"},
    {"_id": 4, "category": "Electronics", "amount": 50, "date": "2023-01-03"},
    {"_id": 5, "category": "Furniture", "amount": 300, "date": "2023-01-02"},
];
coll.drop();
assert.commandWorked(coll.insertMany(docs));

const pipeline = [{
    $setWindowFields: {
        partitionBy: "$category",
        sortBy: {date: 1},
        output: {runningTotal: {$sum: "$amount", window: {documents: ["unbounded", "current"]}}}
    }
}];

// Set slow threshold to -1 to ensure that all operations are logged as SLOW.
assert.commandWorked(testDB.setProfilingLevel(1, {slowms: -1}));

// Run query with increased spilling to spill while creating the first batch.
{
    jsTest.log(`Running spill in first batch`);
    setServerParameter(1024);

    // Retrieve the first batch.
    jsTest.log.info("Running pipeline: ", pipeline[0]);
    const cursor = coll.aggregate(pipeline, {"allowDiskUse": true, cursor: {batchSize: 1}});
    const cursorId = cursor.getId();

    const releaseMemoryCmd = {releaseMemory: [cursorId]};
    jsTest.log.info("Running releaseMemory: ", releaseMemoryCmd);
    const releaseMemoryRes = db.runCommand(releaseMemoryCmd);
    assert.commandWorked(releaseMemoryRes);

    setServerParameter(memoryInitialValue);
}

// spillStorage should be present in the slow query log.
const predicate = new RegExp(`Slow query.*"releaseMemory".*"spillStorage"`);
assert(checkLog.checkContainsOnce(testDB, predicate), "Could not find log containing " + predicate);
