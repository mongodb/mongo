/**
 * Tests for the connection establishment metrics.
 *
 * @tags: [requires_fcv_61, featureFlagConnHealthMetrics]
 */
(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load("jstests/libs/log.js");
load("jstests/libs/parallel_shell_helpers.js");

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
function validateLogAndExtractEntry() {
    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    let queryLogEntry = null;
    for (const line of findMatchingLogLines(mongosLog.log, {id: 6496400})) {
        let entry = JSON.parse(line);
        validateSlowConnectionLogEntry(entry);
        if (entry.attr.totalTimeMillis >= kConnectionEstablishmentDelayMillis) {
            queryLogEntry = entry;
        }
    }
    return queryLogEntry;
}

const kConnectionEstablishmentDelayMillis = 250;
const kDBName = 'TestDB';
const kCollectionName = 'sharded_coll';
const kKeyName = 'foo';

let st = new ShardingTest({shards: 1});

jsTestLog("Setting up the test collection.");

assert.commandWorked(st.s.adminCommand({enableSharding: kDBName}));
assert.commandWorked(
    st.s.adminCommand({shardcollection: `${kDBName}.${kCollectionName}`, key: {[kKeyName]: 1}}));

let db = st.getDB(kDBName);
assert.commandWorked(db[kCollectionName].insertOne({primaryOnly: true, [kKeyName]: 42}));

jsTestLog("Activating the delay in connection establishment.");
let connDelayFailPoint = configureFailPoint(
    st.s, 'transportLayerASIOdelayConnection', {millis: kConnectionEstablishmentDelayMillis});
assert.commandWorked(st.s.adminCommand(
    {setParameter: 1, slowConnectionThresholdMillis: kConnectionEstablishmentDelayMillis}));
assert.commandWorked(
    st.s.adminCommand({dropConnections: 1, hostAndPort: [st.rs0.getPrimary().host]}));

jsTestLog("Running the query.");

function runTestQuery(db) {
    return startParallelShell(
        funWithArgs((host, dbName, collName, keyName) => {
            let conn = new Mongo(host);
            assert.eq(1,
                      conn.getDB(dbName)
                          .getCollection(collName)
                          .find({primaryOnly: true, [keyName]: 42})
                          .itcount());
        }, db.getMongo().host, db.getName(), kCollectionName, kKeyName), null, true);
}
let queryShell = runTestQuery(db);

jsTestLog("Checking the mongos log.");

assert.soon(() => validateLogAndExtractEntry() != null,
            "Slow connection establishment log entry not found.",
            5000,
            250);

queryShell();
connDelayFailPoint.off();

jsTestLog("Checking the output of serverStatus.");
let queryLogEntry = validateLogAndExtractEntry();
let status = assert.commandWorked(st.s.adminCommand({serverStatus: 1}));
printjson(status);
assert.gte(status.metrics.network.totalEgressConnectionEstablishmentTimeMillis,
           queryLogEntry.attr.totalTimeMillis);

st.stop();
})();
