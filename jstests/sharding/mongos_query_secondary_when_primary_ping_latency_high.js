/**
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   multiversion_incompatible,
 * ]
 */
(function() {
'use strict';
load("jstests/libs/fail_point_util.js");
load("jstests/replsets/rslib.js");
load("jstests/libs/log.js");
{
    let st = new ShardingTest({shards: {rs0: {nodes: 2}}});
    let mongos = st.s;
    const replSet = st.rs0;
    const totalReads = 1000;
    const higherLatencyServerReads =
        20;  // keeping a minimal counter of reads that may be go to higher latency primary

    var dbs = st.getDB("test");
    dbs.foo.save({a: 1});

    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();
    const rsPrimary = st.rs0.getPrimary();
    const rsSecondary = st.rs0.getSecondaries()[0];

    // Make sure mongos knows who the primary is
    awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: true});

    const kPingWaitTimeMS =
        32 * 1000;  // streamable ReplicaSetMonitor's ping interval is 10 seconds. give it 3
                    // intervals to adjust primary and secondary server latencies

    // Turning on serverPingMonitorSetRTT on primary and secondary
    const highLatency = 1000000, lowLatency = 10;
    var hosts = {};
    hosts[rsPrimary.host] = highLatency;   // set high latency on primary
    hosts[rsSecondary.host] = lowLatency;  // set low latency on secondary

    const monitorDelayFailPoint = configureFailPoint(mongos, "serverPingMonitorSetRTT", hosts);

    sleep(kPingWaitTimeMS);

    awaitRSClientHosts(mongos, {host: rsPrimary.name}, {ok: true, ismaster: true});

    let beforeReadsPrimaryQueryCount = rsPrimary.adminCommand("serverStatus").opcounters.query;
    let beforeReadsSecondaryQueryCount = rsSecondary.adminCommand("serverStatus").opcounters.query;
    // do totalReads(1000)
    for (var i = 0; i < totalReads; ++i) {
        assert.commandWorked(dbs.runCommand({find: "foo", $readPreference: {mode: "nearest"}}));
    }

    let afterReadsPrimaryQueryCount = rsPrimary.adminCommand("serverStatus").opcounters.query;
    let afterReadsSecondaryQueryCount = rsSecondary.adminCommand("serverStatus").opcounters.query;

    assert.gte(afterReadsSecondaryQueryCount - beforeReadsSecondaryQueryCount,
               totalReads);  // secondary gets all the reads.
    assert.lt(afterReadsPrimaryQueryCount - beforeReadsPrimaryQueryCount,
              higherLatencyServerReads);  // primary may get some queries.

    monitorDelayFailPoint.off();
    st.stop();
}
})();
