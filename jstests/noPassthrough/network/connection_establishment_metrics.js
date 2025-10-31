/**
 * Tests for the connection establishment metrics.
 *
 * @tags: [requires_fcv_63]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {iterateMatchingLogLines} from "jstests/libs/log.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function hasNonNegativeAttr(entry, attrName) {
    return entry.attr.hasOwnProperty(attrName) && entry.attr[attrName] >= 0;
}
function hasNullAttr(entry, attrName) {
    return entry.attr.hasOwnProperty(attrName) && entry.attr[attrName] == null;
}
function hasOptionalMillisAttr(entry, attrName) {
    return hasNullAttr(entry, attrName) || hasNonNegativeAttr(entry, attrName + "Millis");
}
function validateSlowConnectionLogEntry(entry) {
    assert(entry.hasOwnProperty("attr"));
    assert(entry.attr.hasOwnProperty("hostAndPort"));
    assert(hasNonNegativeAttr(entry, "dnsResolutionTimeMillis"));
    assert(hasNonNegativeAttr(entry, "tcpConnectionTimeMillis"));
    assert(hasOptionalMillisAttr(entry, "tlsHandshakeTime"));
    assert(hasNonNegativeAttr(entry, "authTimeMillis"));
    assert(hasOptionalMillisAttr(entry, "hookTime"));
    assert(hasNonNegativeAttr(entry, "totalTimeMillis"));
    assert(hasNonNegativeAttr(entry, "poolConnId"));

    let total = entry.attr.dnsResolutionTimeMillis + entry.attr.tcpConnectionTimeMillis + entry.attr.authTimeMillis;
    if (entry.attr.tlsHandshakeTimeMillis >= 0) {
        total += entry.attr.tlsHandshakeTimeMillis;
    }
    if (entry.attr.hookTimeMillis >= 0) {
        total += entry.attr.hookTimeMillis;
    }
    assert.eq(total, entry.attr.totalTimeMillis);
}

function validateLogAndExtractCountAndEntry(st) {
    const mongosLog = assert.commandWorked(st.s.adminCommand({getLog: "global"}));
    let queryLogEntry = null;

    let count = 0;
    for (const line of iterateMatchingLogLines(mongosLog.log, {id: 6496400})) {
        count++;
        let entry = JSON.parse(line);
        validateSlowConnectionLogEntry(entry);
        if (entry.attr.totalTimeMillis >= kConnectionEstablishmentDelayMillis) {
            queryLogEntry = entry;
        }
    }
    return {count: count, entry: queryLogEntry};
}

const kConnectionEstablishmentDelayMillis = 250;
const kDBName = "TestDB";
const kCollectionName = "sharded_coll";
const kKeyName = "foo";

let runTest = (connectionHealthLoggingOn) => {
    let st = new ShardingTest({shards: 1});

    if (!connectionHealthLoggingOn) {
        assert.commandWorked(st.s.adminCommand({setParameter: 1, enableDetailedConnectionHealthMetricLogLines: false}));
    }

    const initialLogEntryCount = validateLogAndExtractCountAndEntry(st).count;

    jsTestLog("Setting up the test collection.");

    assert.commandWorked(st.s.adminCommand({enableSharding: kDBName}));
    assert.commandWorked(st.s.adminCommand({shardcollection: `${kDBName}.${kCollectionName}`, key: {[kKeyName]: 1}}));

    let db = st.getDB(kDBName);
    assert.commandWorked(db[kCollectionName].insertOne({primaryOnly: true, [kKeyName]: 42}));

    jsTestLog("Activating the delay in connection establishment.");
    let connDelayFailPoint = configureFailPoint(st.s, "asioTransportLayerDelayConnection", {
        millis: kConnectionEstablishmentDelayMillis,
    });
    assert.commandWorked(
        st.s.adminCommand({setParameter: 1, slowConnectionThresholdMillis: kConnectionEstablishmentDelayMillis}),
    );
    assert.commandWorked(st.s.adminCommand({dropConnections: 1, hostAndPort: [st.rs0.getPrimary().name]}));

    jsTestLog("Running the query.");

    function runTestQuery(db) {
        return startParallelShell(
            funWithArgs(
                (host, dbName, collName, keyName) => {
                    let conn = new Mongo(host);
                    assert.eq(
                        1,
                        conn
                            .getDB(dbName)
                            .getCollection(collName)
                            .find({primaryOnly: true, [keyName]: 42})
                            .itcount(),
                    );
                },
                db.getMongo().name,
                db.getName(),
                kCollectionName,
                kKeyName,
            ),
            null,
            true,
        );
    }
    let queryShell = runTestQuery(db);

    let expectedTotalEgressConnectionEstablishmentTimeMillis = 0;
    if (connectionHealthLoggingOn) {
        jsTestLog("Checking the mongos log.");

        assert.soon(
            () => validateLogAndExtractCountAndEntry(st).entry != null,
            "Slow connection establishment log entry not found.",
        );

        let queryLogEntry = validateLogAndExtractCountAndEntry(st).entry;
        expectedTotalEgressConnectionEstablishmentTimeMillis = queryLogEntry.attr.totalTimeMillis;
    } else {
        assert.eq(validateLogAndExtractCountAndEntry(st).count, initialLogEntryCount);
    }

    queryShell();
    connDelayFailPoint.off();

    jsTestLog("Checking the output of serverStatus.");
    let status = assert.commandWorked(st.s.adminCommand({serverStatus: 1}));
    printjson(status);
    assert.gte(
        status.metrics.network.totalEgressConnectionEstablishmentTimeMillis,
        expectedTotalEgressConnectionEstablishmentTimeMillis,
    );
    assert.gt(status.network.dnsResolveStatsLastMin.count, 0);
    // At least one resolve will need to take some time, so mean and max should be gte 0, other resolves may use cached results and be very fast, hence
    // percentiles may be zero.
    assert.gt(status.network.dnsResolveStatsLastMin.meanMillis, 0);
    assert.gt(status.network.dnsResolveStatsLastMin.maxMillis, 0);
    assert.gte(status.network.dnsResolveStatsLastMin.p50Millis, 0);
    assert.gte(status.network.dnsResolveStatsLastMin.p90Millis, 0);
    assert.gte(status.network.dnsResolveStatsLastMin.p99Millis, 0);

    st.stop();
};

// Parameter is connectionHealthLoggingOn == true/false
runTest(true);
runTest(false);
