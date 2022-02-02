/**
 * Tests the Config server observer.
 *
 *  @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

const kWaitForCompletedChecksCount = 20;
const kWaitForPassedChecksCount = 10;

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "configServer", intensity: "critical"},
            ]
        }),
        healthMonitoringIntervals: tojson({values: [{type: "configServer", interval: 200}]}),
        featureFlagHealthMonitoring: true
    }
};

let st = new ShardingTest({
    mongos: [params],
    shards: 1,
});

assert.commandWorked(st.s0.adminCommand(
    {"setParameter": 1, logComponentVerbosity: {processHealth: {verbosity: 3}}}));

// Expects some minimal check count to pass.
assert.soon(() => {
    let result =
        assert.commandWorked(st.s0.adminCommand({serverStatus: 1, health: {details: true}})).health;
    print(`Server status: ${tojson(result)}`);
    // Wait for a certain count of checks completed.
    // At least some checks passed (more than 1).
    if (result.configServer.totalChecks >= kWaitForCompletedChecksCount &&
        result.configServer.totalChecks - result.configServer.totalChecksWithFailure >=
            kWaitForPassedChecksCount) {
        return true;
    }
    return false;
}, 'Config server health check did not reach necessary count of passed checks', 60000, 1000);

st.stop();
})();
