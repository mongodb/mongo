/**
 * Tests for the connection establishment metrics.
 *
 * @tags: [requires_fcv_61, featureFlagConnHealthMetrics]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/log.js");

const testThresholdMillis = 500;  // slow connection threshold for this test

let st = new ShardingTest({shards: 1});

jsTestLog("Setting up the test collection.");

assert.commandWorked(st.s.adminCommand({enableSharding: 'TestDB'}));
assert.commandWorked(st.s.adminCommand({shardcollection: 'TestDB.sharded_coll', key: {foo: 1}}));

let db = st.getDB('TestDB');
assert.commandWorked(db.sharded_coll.insertOne({primaryOnly: true, foo: 42}));

let primary = st.rs0.getPrimary().getDB('TestDB');
let primaryHost = primary.getMongo().host;

jsTestLog("Ensuring the next connection from mongos to primary mongod will hang.");
let hangBeforeAcceptFailPoint =
    configureFailPoint(primary, 'transportLayerASIOhangDuringAcceptCallback');
assert.commandWorked(st.s.adminCommand({dropConnections: 1, hostAndPort: [primaryHost]}));
assert.commandWorked(
    st.s.adminCommand({setParameter: 1, slowConnectionThresholdMillis: testThresholdMillis}));

jsTestLog("Running the query.");

function runTestQuery(db) {
    let queryScript = "let conn = new Mongo(\"" + db.getMongo().host +
        "\"); assert.eq(1, conn.getDB('TestDB').getCollection('sharded_coll').find({primaryOnly: true, foo: 42}).itcount());";
    return startParallelShell(queryScript, null, true);
}
let queryShell = runTestQuery(db);
hangBeforeAcceptFailPoint.wait();
sleep(testThresholdMillis);  // make sure that we hit the slow connection threshold
hangBeforeAcceptFailPoint.off();
queryShell();

jsTestLog("Checking the mongos log.");

function hasNonNegativeAttr(entry, attrName) {
    return entry.attr.hasOwnProperty(attrName) && entry.attr[attrName] >= 0;
}
function hasNullAttr(entry, attrName) {
    return entry.attr.hasOwnProperty(attrName) && entry.attr[attrName] == null;
}
function hasOptionalMillisAttr(entry, attrName) {
    return hasNullAttr(entry, attrName) || hasNonNegativeAttr(entry, attrName + 'Millis');
}
function validateSlowConnectionLogEntry(entry) {
    assert(entry.hasOwnProperty('attr'));
    assert(entry.attr.hasOwnProperty('hostAndPort'));
    assert(hasNonNegativeAttr(entry, 'dnsResolutionTimeMillis'));
    assert(hasNonNegativeAttr(entry, 'tcpConnectionTimeMillis'));
    assert(hasOptionalMillisAttr(entry, 'tlsHandshakeTime'));
    assert(hasNonNegativeAttr(entry, 'authTimeMillis'));
    assert(hasOptionalMillisAttr(entry, 'hookTime'));
    assert(hasNonNegativeAttr(entry, 'totalTimeMillis'));

    let total = entry.attr.dnsResolutionTimeMillis + entry.attr.tcpConnectionTimeMillis +
        entry.attr.authTimeMillis;
    if (entry.attr.tlsHandshakeTimeMillis >= 0) {
        total += entry.attr.tlsHandshakeTimeMillis;
    }
    if (entry.attr.hookTimeMillis >= 0) {
        total += entry.attr.hookTimeMillis;
    }
    assert.eq(total, entry.attr.totalTimeMillis);
}

const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
let queryLogEntry = null;
for (const line of findMatchingLogLines(mongosLog.log, {id: 6496400})) {
    let entry = JSON.parse(line);
    validateSlowConnectionLogEntry(entry);
    if (entry.attr.hostAndPort == primaryHost &&
        entry.attr.totalTimeMillis >= testThresholdMillis) {
        queryLogEntry = entry;
    }
}
assert(queryLogEntry);

jsTestLog("Checking the output of serverStatus.");
let status = assert.commandWorked(st.s.adminCommand({serverStatus: 1}));
printjson(status);
assert.gte(status.metrics.mongos.totalConnectionEstablishmentTimeMillis,
           queryLogEntry.attr.totalTimeMillis);

st.stop();
})();
