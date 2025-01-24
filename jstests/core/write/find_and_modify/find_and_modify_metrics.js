/**
 * Tests "metrics.commands.findAndModify.pipeline", "metrics.commands.findAndModify.arrayFilters"
 * and "metrics.document" counters of the findAndModify command.
 *
 * @tags: [
 *   # The test relies on the precise number of executions of commands.
 *   requires_non_retryable_writes,
 *   # The test is designed to work with an unsharded collection.
 *   assumes_unsharded_collection,
 *   # The config fuzzer may run logical session cache refreshes in the background, which modifies
 *   # some serverStatus metrics read in this test.
 *   does_not_support_config_fuzzer,
 *   # Multi clients run concurrently and may modify the serverStatus metrices read in this test.
 *   multi_clients_incompatible,
 *   # The metrics checked in the test may be spoiled by data movement.
 *   assumes_balancer_off,
 * ]
 */
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

// Verify that findAndModify command increments document metrics correctly.
// Update and return no document
serverStatusBeforeTest = testDB.serverStatus();
result = coll.findAndModify({query: {a: 2}, update: {$set: {b: 2}}});
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.document.updated,
          serverStatusAfterTest.metrics.document.updated,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);
assert.eq(serverStatusBeforeTest.metrics.document.returned,
          serverStatusAfterTest.metrics.document.returned,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

// Update and return a single document
assert.commandWorked(coll.insert([{a: 1, b: 1}]));
serverStatusBeforeTest = testDB.serverStatus();
result = coll.findAndModify({query: {a: 1}, update: {$set: {b: 2}}});
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.document.updated + 1,
          serverStatusAfterTest.metrics.document.updated,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);
assert.eq(serverStatusBeforeTest.metrics.document.returned + 1,
          serverStatusAfterTest.metrics.document.returned,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

// Delete and return no document
serverStatusBeforeTest = testDB.serverStatus();
result = coll.findAndModify({query: {a: 2}, remove: true});
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.document.deleted,
          serverStatusAfterTest.metrics.document.deleted,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);
assert.eq(serverStatusBeforeTest.metrics.document.returned,
          serverStatusAfterTest.metrics.document.returned,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);

// Delete and return a single document
serverStatusBeforeTest = testDB.serverStatus();
result = coll.findAndModify({query: {a: 1}, remove: true});
serverStatusAfterTest = testDB.serverStatus();
assert.eq(serverStatusBeforeTest.metrics.document.deleted + 1,
          serverStatusAfterTest.metrics.document.deleted,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);
assert.eq(serverStatusBeforeTest.metrics.document.returned + 1,
          serverStatusAfterTest.metrics.document.returned,
          `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`);
