/**
 * Tests "metrics.commands.findAndModify.pipeline" and "metrics.commands.findAndModify.arrayFilters"
 * counters of the findAndModify command.
 *
 * @tags: [
 *   # The test relies on the precise number of executions of commands.
 *   requires_non_retryable_writes,
 *   # The test is designed to work with an unsharded collection.
 *   assumes_unsharded_collection,
 *   # This test contains assertions on the number of executed operations, and tenant migrations
 *   # passthrough suites automatically retry operations on TenantMigrationAborted errors.
 *   tenant_migration_incompatible,
 *   # The config fuzzer may run logical session cache refreshes in the background, which modifies
 *   # some serverStatus metrics read in this test.
 *   does_not_support_config_fuzzer,
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.findAndModify_metrics;
assert.commandWorked(testDB.createCollection(coll.getName()));

assert.commandWorked(coll.insert([{key: 1, value: 1, array: [5, 10]}]));

// "Initialize" the counters for the findAndModify command.
let result = coll.findAndModify({query: {key: 1}, update: {$set: {value: 0}}});
assert.eq(1, result.key);

let serverStatusBeforeTest = testDB.serverStatus();

// Verify that the metrics.commands.findAndModify.pipeline counter is present.
assert.gte(serverStatusBeforeTest.metrics.commands.findAndModify.pipeline,
           0,
           tojson(serverStatusBeforeTest));

// Verify that that findAndModify command without aggregation pipeline-style update does not
// increment the counter.
result = coll.findAndModify({query: {key: 1}, update: {$set: {value: 5}}});
assert.eq(1, result.key);
let serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands.findAndModify.pipeline,
          serverStatusAfterTest.metrics.commands.findAndModify.pipeline,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

// Verify that that findAndModify command with aggregation pipeline-style update increments the
// counter.
result = coll.findAndModify({query: {key: 1}, update: [{$set: {value: 10}}]});
assert.eq(1, result.key);
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands.findAndModify.pipeline + 1,
          serverStatusAfterTest.metrics.commands.findAndModify.pipeline,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

serverStatusBeforeTest = testDB.serverStatus();

// Verify that the metrics.commands.findAndModify.arrayFilters counter is present.
assert.gte(serverStatusBeforeTest.metrics.commands.findAndModify.arrayFilters,
           0,
           tojson(serverStatusBeforeTest));

// Verify that that findAndModify command without arrayFilters does not increment the counter.
result = coll.findAndModify({query: {key: 1}, update: {$set: {value: 5}}});
assert.eq(1, result.key);
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands.findAndModify.arrayFilters,
          serverStatusAfterTest.metrics.commands.findAndModify.arrayFilters,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

// Verify that that findAndModify command with arrayFilters increments the counter.
result = coll.findAndModify({
    query: {key: 1},
    update: {$set: {"array.$[element]": 20}},
    arrayFilters: [{"element": {$gt: 6}}]
});
assert.eq(1, result.key);
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands.findAndModify.arrayFilters + 1,
          serverStatusAfterTest.metrics.commands.findAndModify.arrayFilters,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);
})();
