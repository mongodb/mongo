/**
 * Tests that if the primary config server is blackholed from the point of view of mongos,
 * the Config server health checker will eventually crash the server.
 *
 *  @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

load('jstests/replsets/rslib.js');

const kActiveFaultDurationSec = 12;

// Crashed mongos will remain holding its socket as a zombie for some time.
TestData.failIfUnterminatedProcesses = false;

// Checking index consistency involves talking to the primary config server which is blackholed from
// the mongos in this test.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

const mongosParams = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "configServer", intensity: "critical"},
            ]
        }),
        healthMonitoringIntervals: tojson({values: [{type: "configServer", interval: 1}]}),
        activeFaultDurationSecs: kActiveFaultDurationSec,
        featureFlagHealthMonitoring: true
    }
};

const faultState = function() {
    let result =
        assert.commandWorked(st.s0.adminCommand({serverStatus: 1, health: {details: true}})).health;
    print(`Server status: ${tojson(result)}`);
    return result.state;
};

var st = new ShardingTest({
    shards: 1,
    mongos: [mongosParams, {}],
    other: {useBridge: true},
});

assert.commandWorked(st.s0.adminCommand(
    {"setParameter": 1, logComponentVerbosity: {processHealth: {verbosity: 3}}}));

const configPrimary = st.configRS.getPrimary();
const admin = configPrimary.getDB("admin");

// Set the priority and votes to 0 for secondary config servers so that in the case
// of an election, they cannot step up. If a different node were to step up, the
// config server would no longer be blackholed from mongos.
let conf = admin.runCommand({replSetGetConfig: 1}).config;
for (let i = 0; i < conf.members.length; i++) {
    if (conf.members[i].host !== configPrimary.host) {
        conf.members[i].votes = 0;
        conf.members[i].priority = 0;
    }
}
reconfig(st.configRS, conf);
jsTest.log('Partitioning a config server replica from the mongos');
st.config0.discardMessagesFrom(st.s, 1.0);
st.s.discardMessagesFrom(st.config0, 1.0);
sleep(1000);

// Blocking only one config replica may sometimes transfer to the transient fault.
assert.soon(() => {
    return faultState() == 'Ok';
}, 'Mongos not transitioned to fault state', 12000, 100);

jsTest.log('Partitioning another config server replica from the mongos');
st.config1.discardMessagesFrom(st.s, 1.0);
st.s.discardMessagesFrom(st.config1, 1.0);

const failedChecksCount = function() {
    let result =
        assert.commandWorked(st.s0.adminCommand({serverStatus: 1, health: {details: true}})).health;
    print(`Server status: ${tojson(result)}`);
    return result.configServer.totalChecksWithFailure;
};

// Wait for certain count of checks that detected a failure, or network error.
assert.soon(() => {
    try {
        // Checks that the failure can be detected more than once.
        return failedChecksCount() > 1;
    } catch (e) {
        jsTestLog(`Can't fetch server status: ${e}`);
        return true;  // Server must be down already.
    }
}, 'Health observer did not detect several failures', 40000, 1000, {runHangAnalyzer: false});

// Mongos should not crash yet.
assert.commandWorked(st.s0.adminCommand({"ping": 1}));

jsTest.log('Partitioning the final config server replica from the mongos');
st.config2.discardMessagesFrom(st.s, 1.0);
st.s.discardMessagesFrom(st.config2, 1.0);

// Asserts that the Config server health observer will eventually trigger mongos crash.

jsTestLog('Wait until the mongos crashes.');
assert.soon(() => {
    try {
        let res = st.s0.adminCommand({"ping": 1});
        jsTestLog(`Ping result: ${tojson(res)}`);
        return res.ok != 1;
    } catch (e) {
        jsTestLog(`Ping failed: ${tojson(e)}`);
        return true;
    }
}, 'Mongos is not shutting down as expected', 40000, 400);

try {
    st.stop({skipValidatingExitCode: true, skipValidation: true});
} catch (e) {
    jsTestLog(`Exception during shutdown: ${e}`);
}
}());
