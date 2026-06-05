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

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// This test makes assertions on the "arrayFilters" metric from the serverStatus, which is mongos specific.
// pinToSingleMongos due to serverStatus command with "arrayFilters" metric.
TestData.pinToSingleMongos = true;

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
const updateField = TestData.runningWithBulkWriteOverride ? "bulkWrite" : "update";

// Verify that the metrics.commands[updateField].pipeline counter is present.
assert.gte(serverStatusBeforeTest.metrics.commands[updateField].pipeline, 0, tojson(serverStatusBeforeTest));

// Verify that that update command without aggregation pipeline-style update does not increment the
// counter.
assert.commandWorked(coll.update({key: 1}, {$set: {value: 5}}));
let serverStatusAfterTest = testDB.serverStatus();
assert.eq(
    serverStatusBeforeTest.metrics.commands[updateField].pipeline,
    serverStatusAfterTest.metrics.commands[updateField].pipeline,
    `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`,
);

// Verify that that update command with aggregation pipeline-style update increments the counter.
assert.commandWorked(coll.update({key: 1}, [{$set: {value: 10}}]));
serverStatusAfterTest = testDB.serverStatus();
assert.eq(
    serverStatusBeforeTest.metrics.commands[updateField].pipeline + 1,
    serverStatusAfterTest.metrics.commands[updateField].pipeline,
    `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`,
);

serverStatusBeforeTest = testDB.serverStatus();

// Verify that the metrics.commands[updateField].arrayFilters counter is present.
assert.gte(serverStatusBeforeTest.metrics.commands[updateField].arrayFilters, 0, tojson(serverStatusBeforeTest));

// Verify that that update command without arrayFilters does not increment the counter.
assert.commandWorked(coll.update({key: 1}, {$set: {value: 5}}));
serverStatusAfterTest = testDB.serverStatus();
assert.eq(
    serverStatusBeforeTest.metrics.commands[updateField].arrayFilters,
    serverStatusAfterTest.metrics.commands[updateField].arrayFilters,
    `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`,
);

// Verify that that update command with arrayFilters increments the counter.
assert.commandWorked(coll.update({key: 1}, {$set: {"array.$[element]": 20}}, {arrayFilters: [{"element": {$gt: 6}}]}));
serverStatusAfterTest = testDB.serverStatus();
assert.eq(
    serverStatusBeforeTest.metrics.commands[updateField].arrayFilters + 1,
    serverStatusAfterTest.metrics.commands[updateField].arrayFilters,
    `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`,
);

// Verify that that a multi-document update command with arrayFilters increments the counter.
assert.commandWorked(
    coll.insert([
        {key: 2, value: 1, array: [7, 0]},
        {key: 3, value: 1, array: [7, 0]},
    ]),
);
assert.commandWorked(
    coll.update({}, {$set: {"array.$[element]": 20}}, {multi: true, arrayFilters: [{"element": {$gt: 6}}]}),
);
serverStatusAfterTest = testDB.serverStatus();
assert.eq(
    serverStatusBeforeTest.metrics.commands[updateField].arrayFilters + 2,
    serverStatusAfterTest.metrics.commands[updateField].arrayFilters,
    `Before:  ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`,
);

// Cover the same counters when the target is a timeseries collection. Both viewful and viewless
// timeseries take the same isTimeseriesLogicalRequest=true path through CmdUpdate, so a
// non-retryable update with rawData unset must still bump the metrics. Skip when arbitrary
// timeseries updates aren't supported (older binaries / multiversion suites).
if (FeatureFlagUtil.isPresentAndEnabled(testDB, "TimeseriesUpdatesSupport")) {
    const tsColl = testDB.update_metrics_timeseries;
    assert.commandWorked(testDB.createCollection(tsColl.getName(), {timeseries: {timeField: "t", metaField: "m"}}));
    assert.commandWorked(
        tsColl.insert([
            {t: ISODate(), m: 1, value: 1, array: [5, 10]},
            {t: ISODate(), m: 1, value: 2, array: [5, 10]},
        ]),
    );

    // Pipeline-style update on a timeseries collection should bump the pipeline counter.
    serverStatusBeforeTest = testDB.serverStatus();
    assert.commandWorked(tsColl.update({m: 1}, [{$set: {value: 99}}], {multi: true}));
    serverStatusAfterTest = testDB.serverStatus();
    assert.eq(
        serverStatusBeforeTest.metrics.commands[updateField].pipeline + 1,
        serverStatusAfterTest.metrics.commands[updateField].pipeline,
        `timeseries pipeline update; before: ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`,
    );

    // arrayFilters update on a timeseries collection should bump the arrayFilters counter.
    serverStatusBeforeTest = testDB.serverStatus();
    assert.commandWorked(
        tsColl.update({m: 1}, {$set: {"array.$[element]": 42}}, {multi: true, arrayFilters: [{"element": {$gt: 6}}]}),
    );
    serverStatusAfterTest = testDB.serverStatus();
    assert.eq(
        serverStatusBeforeTest.metrics.commands[updateField].arrayFilters + 1,
        serverStatusAfterTest.metrics.commands[updateField].arrayFilters,
        `timeseries arrayFilters update; before: ${tojson(serverStatusBeforeTest)}, after: ${tojson(serverStatusAfterTest)}`,
    );
}
