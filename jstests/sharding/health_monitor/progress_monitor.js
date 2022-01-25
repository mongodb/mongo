/*
 *  @tags: [multiversion_incompatible]
 */
const PROGRESS_TIMEOUT_SECONDS = 5;
const monitoringIntervalMs = 500;
(function() {
'use strict';

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "test", intensity: "critical"},
            ]
        }),
        healthMonitoringIntervals:
            tojson({values: [{type: "test", interval: NumberInt(monitoringIntervalMs)}]}),
        progressMonitor:
            tojson({interval: PROGRESS_TIMEOUT_SECONDS * 1000, deadline: PROGRESS_TIMEOUT_SECONDS}),
        featureFlagHealthMonitoring: true
    }
};
let st = new ShardingTest({
    mongos: [params, params],
    shards: 1,
});
// After cluster startup, make sure both mongos's are available.
assert.commandWorked(st.s0.adminCommand({"ping": 1}));
assert.commandWorked(st.s1.adminCommand({"ping": 1}));
assert.commandWorked(st.s1.adminCommand(
    {"setParameter": 1, logComponentVerbosity: {processHealth: {verbosity: 2}}}));

// Set the failpoint on one of the mongos's to pause its healthchecks.
jsTestLog("hang test health observer on " + st.s1.host);
assert.commandWorked(
    st.s1.adminCommand({"configureFailPoint": 'hangTestHealthObserver', "mode": "alwaysOn"}));

// Wait for the progress monitor timeout to elapse.
sleep(1.1 * PROGRESS_TIMEOUT_SECONDS * 1000);
jsTestLog("Done sleeping");

assert.soon(() => {
    try {
        assert.commandWorked(st.s0.adminCommand({"ping": 1}));  // Ensure s0 is unaffected.
        st.s1.adminCommand(
            {"ping": 1});  // This should throw an error because s1 is no longer reachable.
        assert(false, "ping command to s1 should fail.");
    } catch (e) {
        // This might seem brittle to rely on the string message for the error, but the same check
        // appears in the implementation for runCommand().
        if (e.message.indexOf("network error") >= 0) {
            return true;
        } else {
            jsTestLog(`Failure: ${e}`);
            sleep(1000);
            return false;
        }
    }
    sleep(1000);
    return false;
}, "Pinging faulty mongos should fail with network error.");
// Don't validate exit codes, since a mongos will exit on its own with a non-zero exit code.

st.stop({skipValidatingExitCode: true});
})();
