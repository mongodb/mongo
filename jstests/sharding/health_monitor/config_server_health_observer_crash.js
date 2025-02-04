/**
 * Tests that if the primary config server is blackholed from the point of view of mongos,
 * the Config server health checker will eventually crash the server.
 *
 *  @tags: [
 *    multiversion_incompatible,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 *    # TODO (SERVER-94095): Re-enable this test in aubsan and tsan once DEVPROD-10102 is resolved.
 *    incompatible_aubsan,
 *    incompatible_tsan,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconfig} from "jstests/replsets/rslib.js";

const kActiveFaultDurationSec = 12;

// Crashed mongos will remain holding its socket as a zombie for some time.
TestData.ignoreUnterminatedProcesses = true;
// Because this test intentionally causes the server to crash, we need to instruct the
// shell to clean up the core dump that is left behind.
TestData.cleanUpCoreDumpsFromExpectedCrash = true;

// Checking index consistency involves talking to the primary config server which is blackholed from
// the mongos in this test.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;
TestData.skipCheckShardFilteringMetadata = true;

const mongosParams = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "configServer", intensity: "critical"},
            ]
        }),
        healthMonitoringIntervals: tojson({values: [{type: "configServer", interval: 1}]}),
        activeFaultDurationSecs: kActiveFaultDurationSec,
    }
};

const assertFaultState = function(state) {
    let result =
        assert.commandWorked(st.s0.adminCommand({serverStatus: 1, health: {details: true}})).health;
    print(`Server health: ${tojson(result)}`);
    assert(result.state == state);
};

var st = new ShardingTest({
    shards: 1,
    mongos: [mongosParams, {}],
    other: {useBridge: true},
    config: 3,
});

assert.commandWorked(st.s0.adminCommand(
    {"setParameter": 1, logComponentVerbosity: {processHealth: {verbosity: 3}}}));

const configPrimary = st.configRS.getPrimary();
const admin = configPrimary.getDB("admin");

// There should be no faults and we should be able to ping mongos.
assertFaultState('Ok');
assert.commandWorked(st.s0.adminCommand({"ping": 1}));

let pidsBefore = _runningMongoChildProcessIds();
let numPidsBefore = pidsBefore.length;

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

jsTest.log('Partitioning another config server replica from the mongos');
st.config1.discardMessagesFrom(st.s, 1.0);
st.s.discardMessagesFrom(st.config1, 1.0);

jsTest.log('Partitioning the final config server replica from the mongos');
st.config2.discardMessagesFrom(st.s, 1.0);
st.s.discardMessagesFrom(st.config2, 1.0);

const failedChecksCount = function() {
    let result =
        assert.commandWorked(st.s0.adminCommand({serverStatus: 1, health: {details: true}})).health;
    print(`Server status: ${tojson(result)}`);
    return result.configServer.totalChecksWithFailure && result.state == 'TransientFault';
};

// Wait until a failure is detected.
assert.soon(() => {
    return failedChecksCount();
}, 'Health observer did not detect a failure', 20000, 1000, {runHangAnalyzer: false});

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
}, 'Mongos is not shutting down as expected', 30000, 2500);

try {
    // Refresh PIDs to force de-registration of the crashed mongos.
    assert.soon(
        () => {
            var numPidsNow = _runningMongoChildProcessIds().length;
            return (numPidsBefore - numPidsNow) == 1;
        },
        () => {
            var pids = _runningMongoChildProcessIds();
            return `Encountered incorrect number of running processes. Expected: 11. Running processes: ${
                tojson(pids)}`;
        });
    var pidsNow = _runningMongoChildProcessIds();
    pidsBefore = pidsBefore.map((e) => e.toNumber());
    pidsNow = pidsNow.map((e) => e.toNumber());
    var difference = pidsBefore.filter((element) => !pidsNow.includes(element));
    waitProgram(difference[0]);
    st.stop({skipValidatingExitCode: true, skipValidation: true});
} catch (e) {
    jsTestLog(`Exception during shutdown: ${e}`);
}
