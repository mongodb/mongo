const PROGRESS_TIMEOUT_SECONDS = 5;
const CHECK_PING_SECONDS = 1;
(function() {
'use strict';

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({test: "non-critical", ldap: "off", dns: "off"}),
        healthMonitoringIntervals: tojson({test: 500}),
        progressMonitor: tojson({deadline: PROGRESS_TIMEOUT_SECONDS}),
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
            throw (e);
        }
    }
}, "Pinging faulty mongos should fail with network error.", PROGRESS_TIMEOUT_SECONDS * 1000);
// Don't validate exit codes, since a mongos will exit on its own with a non-zero exit code.

st.stop({skipValidatingExitCode: true});
})();
