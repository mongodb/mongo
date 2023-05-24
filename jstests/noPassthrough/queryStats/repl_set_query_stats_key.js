/**
 * This test confirms that queryStats store key fields specific to replica sets (readConcern and
 * readPreference) are included and correctly shapified. General command fields related to api
 * versioning are included for good measure.
 * @tags: [featureFlagQueryStats]
 */
load("jstests/libs/telemetry_utils.js");
(function() {
"use strict";

const replTest = new ReplSetTest({name: 'reindexTest', nodes: 2});

// Turn on the collecting of telemetry metrics.
replTest.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
replTest.initiate();

const primary = replTest.getPrimary();
const secondary = replTest.getSecondary();

const dbName = jsTestName();
const collName = "foobar";
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB.getCollection(collName);
const secondaryDB = secondary.getDB(dbName);
const secondaryColl = secondaryDB.getCollection(collName);

primaryColl.drop();

assert.commandWorked(primaryColl.insert({a: 1000}));

replTest.awaitReplication();

function confirmCommandFieldsPresent(queryStatsKey, commandObj) {
    for (const field in queryStatsKey) {
        if (field == "queryShape" || field == "applicationName" || field == "command") {
            continue;
        }
        assert(commandObj.hasOwnProperty(field), field);
    }
    assert.eq(Object.keys(queryStatsKey).length, Object.keys(commandObj).length, queryStatsKey);
}

let commandObj = {
    find: collName,
    filter: {v: {$eq: 2}},
    readConcern: {level: "local", afterClusterTime: new Timestamp(0, 1)},
    $readPreference: {mode: "primary"},
    apiDeprecationErrors: false,
    apiVersion: "1",
    apiStrict: false,
};
const replSetConn = new Mongo(replTest.getURL());
assert.commandWorked(replSetConn.getDB(dbName).runCommand(commandObj));
let telemetry = getTelemetryReplSet(replSetConn, collName);
confirmCommandFieldsPresent(telemetry[0].key, commandObj);
// check that readConcern afterClusterTime is normalized.
assert.eq(telemetry[0].key.readConcern.afterClusterTime, "?timestamp");

// check that readPreference not populated and readConcern just has an afterClusterTime field.
commandObj["readConcern"] = {
    afterClusterTime: new Timestamp(1, 0)
};
delete commandObj["$readPreference"];
assert.commandWorked(replSetConn.getDB(dbName).runCommand(commandObj));
telemetry = getTelemetryReplSet(replSetConn, collName);
confirmCommandFieldsPresent(telemetry[0].key, commandObj);
assert.eq(telemetry[0].key["readConcern"], {"afterClusterTime": "?timestamp"});

// check that readConcern has no afterClusterTime and fields related to api usage are not present.
commandObj["readConcern"] = {
    level: "local"
};
delete commandObj["apiDeprecationErrors"];
delete commandObj["apiVersion"];
delete commandObj["apiStrict"];
assert.commandWorked(replSetConn.getDB(dbName).runCommand(commandObj));
telemetry = getTelemetryReplSet(replSetConn, collName);
confirmCommandFieldsPresent(telemetry[2].key, commandObj);
assert.eq(telemetry[2].key["readConcern"], {level: "local"});

replTest.stopSet();
})();
