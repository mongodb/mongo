/*
 *  @tags: [multiversion_incompatible]
 */
const PROGRESS_TIMEOUT_SECONDS = 5;
const CHECK_PING_SECONDS = 1;
(function() {
'use strict';

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "test", intensity: "non-critical"},
                {type: "ldap", intensity: "off"},
                {type: "dns", intensity: "off"}
            ]
        }),
        healthMonitoringIntervals: tojson({values: [{type: "test", interval: NumberInt(500)}]}),
        progressMonitor:
            tojson({interval: PROGRESS_TIMEOUT_SECONDS, deadline: PROGRESS_TIMEOUT_SECONDS}),
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
assert.commandWorked(
    st.s1.adminCommand({"configureFailPoint": 'hangTestHealthObserver', "mode": "alwaysOn"}));
sleep(CHECK_PING_SECONDS * 1000);
// Make sure the failpoint on its own doesn't bring down the server.
assert.commandWorked(st.s1.adminCommand({"ping": 1}));
// Wait for the progress monitor timeout to elapse.
sleep(PROGRESS_TIMEOUT_SECONDS * 1000);

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
}, "Pinging faulty mongos should fail with network error.", PROGRESS_TIMEOUT_SECONDS * 1000);
// Don't validate exit codes, since a mongos will exit on its own with a non-zero exit code.

st.stop({skipValidatingExitCode: true});
})();
