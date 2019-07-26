/**
 * The FTDC connection pool stats from mongos are a different structure than the connPoolStats
 * command, verify its contents.
 *
 * @tags: [requires_sharding]
 */
load('jstests/libs/ftdc.js');

(function() {
'use strict';
const testPath = MongoRunner.toRealPath('ftdc_dir');
const st = new ShardingTest({
    shards: 2,
    mongos: {
        s0: {setParameter: {diagnosticDataCollectionDirectoryPath: testPath}},
    }
});

const admin = st.s0.getDB('admin');
const stats = verifyGetDiagnosticData(admin).connPoolStats;
jsTestLog(`Diagnostic connection pool stats: ${tojson(stats)}`);

assert(stats.hasOwnProperty('totalInUse'));
assert(stats.hasOwnProperty('totalAvailable'));
assert(stats.hasOwnProperty('totalCreated'));
assert(stats.hasOwnProperty('totalRefreshing'));

// The connPoolStats command reply has "hosts", but FTDC's stats do not.
assert(!stats.hasOwnProperty('hosts'));

// Check a few properties, without attempting to be thorough.
assert(stats.connectionsInUsePerPool.hasOwnProperty('NetworkInterfaceTL-ShardRegistry'));
assert(stats.replicaSetPingTimesMillis.hasOwnProperty(st.configRS.name));

st.stop();
})();
