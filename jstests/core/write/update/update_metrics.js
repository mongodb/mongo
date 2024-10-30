/**
 * Tests "metrics.commands.update.pipeline" and "metrics.commands.update.arrayFilters" counters of
 * the update command.
 *
 * @tags: [
 *   # The test is designed to work with an unsharded collection.
 *   assumes_unsharded_collection,
 *   # The config fuzzer may run logical session cache refreshes in the background, which modifies
 *   # some serverStatus metrics read in this test.
 *   does_not_support_config_fuzzer,
 *   # The test relies on the precise number of executions of commands.
 *   requires_non_retryable_writes,
 *   # Multi clients may modify the serverStatus metrices read in this test.
 *   multi_clients_incompatible,
 * ]
 */
const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.update_metrics;
assert.commandWorked(testDB.createCollection(coll.getName()));

assert.commandWorked(coll.insert([{key: 1, value: 1, array: [5, 10]}]));

// "Initialize" the counters for the update command.
assert.commandWorked(coll.update({key: 1}, {$set: {value: 0}}));

let serverStatusBeforeTest = testDB.serverStatus();

// bulkWrite handles UpdateMetrics but it puts them in in
// serverStatus.metrics.commands.bulkWrite instead of
// serverStatus.metrics.commands.update.
const updateField = TestData.runningWithBulkWriteOverride ? 'bulkWrite' : 'update';

// Verify that the metrics.commands[updateField].pipeline counter is present.
assert.gte(serverStatusBeforeTest.metrics.commands[updateField].pipeline,
           0,
           tojson(serverStatusBeforeTest));

// Verify that that update command without aggregation pipeline-style update does not increment the
// counter.
assert.commandWorked(coll.update({key: 1}, {$set: {value: 5}}));
let serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands[updateField].pipeline,
          serverStatusAfterTest.metrics.commands[updateField].pipeline,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

// Verify that that update command with aggregation pipeline-style update increments the counter.
assert.commandWorked(coll.update({key: 1}, [{$set: {value: 10}}]));
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands[updateField].pipeline + 1,
          serverStatusAfterTest.metrics.commands[updateField].pipeline,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

serverStatusBeforeTest = testDB.serverStatus();

// Verify that the metrics.commands[updateField].arrayFilters counter is present.
assert.gte(serverStatusBeforeTest.metrics.commands[updateField].arrayFilters,
           0,
           tojson(serverStatusBeforeTest));

// Verify that that update command without arrayFilters does not increment the counter.
assert.commandWorked(coll.update({key: 1}, {$set: {value: 5}}));
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands[updateField].arrayFilters,
          serverStatusAfterTest.metrics.commands[updateField].arrayFilters,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

// Verify that that update command with arrayFilters increments the counter.
assert.commandWorked(coll.update(
    {key: 1}, {$set: {"array.$[element]": 20}}, {arrayFilters: [{"element": {$gt: 6}}]}));
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands[updateField].arrayFilters + 1,
          serverStatusAfterTest.metrics.commands[updateField].arrayFilters,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

// Verify that that a multi-document update command with arrayFilters increments the counter.
assert.commandWorked(
    coll.insert([{key: 2, value: 1, array: [7, 0]}, {key: 3, value: 1, array: [7, 0]}]));
assert.commandWorked(coll.update(
    {}, {$set: {"array.$[element]": 20}}, {multi: true, arrayFilters: [{"element": {$gt: 6}}]}));
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.commands[updateField].arrayFilters + 2,
          serverStatusAfterTest.metrics.commands[updateField].arrayFilters,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);
