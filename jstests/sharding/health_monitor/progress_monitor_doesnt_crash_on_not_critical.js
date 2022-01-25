/*
 *  @tags: [multiversion_incompatible]
 */
const PROGRESS_TIMEOUT_SECONDS = 5;
(function() {
'use strict';

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "test", intensity: "non-critical"},
            ]
        }),
        healthMonitoringIntervals: tojson({values: [{type: "test", interval: NumberInt(500)}]}),
        progressMonitor:
            tojson({interval: PROGRESS_TIMEOUT_SECONDS * 1000, deadline: PROGRESS_TIMEOUT_SECONDS}),
        featureFlagHealthMonitoring: true
    }
};
let st = new ShardingTest({
    mongos: [params, params],
    shards: 1,
});

function changeObserverIntensity(conn, observer, intensity) {
    let paramValue = {"values": [{"type": observer, "intensity": intensity}]};
    assert.commandWorked(
        conn.adminCommand({"setParameter": 1, healthMonitoringIntensities: paramValue}));
}

// After cluster startup, make sure both mongos's are available.
let pingServers = (servers) => {
    servers.forEach((server) => {
        assert.commandWorked(server.adminCommand({"ping": 1}));
    });
};

pingServers([st.s0, st.s1]);
assert.commandWorked(st.s1.adminCommand(
    {"setParameter": 1, logComponentVerbosity: {processHealth: {verbosity: 2}}}));

jsTestLog("Block test health observer on " + st.s1.host);
assert.commandWorked(
    st.s1.adminCommand({"configureFailPoint": 'hangTestHealthObserver', "mode": "alwaysOn"}));

// Wait for the progress monitor timeout to elapse.
sleep(1.1 * PROGRESS_TIMEOUT_SECONDS * 1000);

// Servers should be still be alive.
jsTestLog("Expect monogs processes to still be alive");
pingServers([st.s0, st.s1]);

jsTestLog("Change observer to critical");
changeObserverIntensity(st.s1, 'test', 'critical');

// Wait for the progress monitor timeout to elapse.
sleep(1.1 * PROGRESS_TIMEOUT_SECONDS * 1000);

jsTestLog("Done sleeping");
// Servers should be still be alive.
pingServers([st.s0]);

let pingNetworkError = false;
try {
    pingServers([st.s1]);
} catch (ex) {
    if (ex.message.indexOf("network error") >= 0) {
        print("Got expected error: " + tojson(ex));
        pingNetworkError = true;
    } else {
        jsTestLog(`Unexpected failure: ${ex}`);
        assert(false, "expected ping on " + st.s1.host + " to fail with network error.");
    }
}
assert(pingNetworkError, "expected " + st.s1.host + "to be killed");

// Don't validate exit codes, since a mongos will exit on its own with a non-zero exit code.
st.stop({skipValidatingExitCode: true});
})();
