(function() {
'use strict';

let CUSTOM_INTERVAL = 1337;
let CUSTOM_DEADLINE = 5;

var st = new ShardingTest({
    mongos: [
        {
            setParameter: {
                healthMonitoringIntensities: tojson({dns: "off", ldap: "critical", test: "off"}),
            }
        },
        {
            setParameter: {
                healthMonitoringIntensities: tojson({dns: "off", ldap: "off"}),
                progressMonitor: tojson({interval: CUSTOM_INTERVAL, deadline: CUSTOM_DEADLINE}),
                healthMonitoringIntervals: tojson({test: CUSTOM_INTERVAL})
            }
        }
    ],
    shards: 1,
});

// Intensity parameter
let result = st.s0.adminCommand({"getParameter": 1, "healthMonitoringIntensities": 1});
assert.eq(result.healthMonitoringIntensities.dns, "off");
assert.eq(result.healthMonitoringIntensities.ldap, "critical");

assert.commandFailed(
    st.s0.adminCommand({"setParameter": 1, healthMonitoringIntensities: {dns: "INVALID"}}));
assert.commandFailed(
    st.s0.adminCommand({"setParameter": 1, healthMonitoringIntensities: {invalid: "off"}}));

assert.commandWorked(st.s0.adminCommand(
    {"setParameter": 1, healthMonitoringIntensities: {dns: 'non-critical', ldap: 'off'}}));
result =
    assert.commandWorked(st.s0.adminCommand({"getParameter": 1, healthMonitoringIntensities: 1}));
assert.eq(result.healthMonitoringIntensities.dns, "non-critical");
assert.eq(result.healthMonitoringIntensities.ldap, "off");

// Interval parameter
result = st.s1.adminCommand({"getParameter": 1, "healthMonitoringIntervals": 1});
assert.eq(result.healthMonitoringIntervals.test, CUSTOM_INTERVAL);

assert.commandFailed(st.s1.adminCommand({"setParameter": 1, healthMonitoringIntervals: {dns: 0}}));
assert.commandFailed(
    st.s1.adminCommand({"setParameter": 1, healthMonitoringIntervals: {invalid: 1000}}));

assert.commandWorked(st.s1.adminCommand({
    "setParameter": 1,
    healthMonitoringIntervals: {dns: NumberInt(2000), ldap: NumberInt(600000)}
}));
result =
    assert.commandWorked(st.s1.adminCommand({"getParameter": 1, healthMonitoringIntervals: 1}));
assert.eq(result.healthMonitoringIntervals.dns, 2000);
assert.eq(result.healthMonitoringIntervals.ldap, 600000);

// Check that custom liveness values were set properly.
result = st.s1.adminCommand({"getParameter": 1, "progressMonitor": 1});
assert.eq(result.progressMonitor.interval, CUSTOM_INTERVAL);
assert.eq(result.progressMonitor.deadline, CUSTOM_DEADLINE);

// Validation tests: intervals must be > 0.
assert.commandFailed(st.s1.adminCommand({"setParameter": 1, progressMonitor: {interval: 0}}));
assert.commandFailed(st.s1.adminCommand({"setParameter": 1, progressMonitor: {interval: -5}}));
assert.commandFailed(st.s1.adminCommand({"setParameter": 1, progressMonitor: {deadline: 0}}));
assert.commandFailed(st.s1.adminCommand({"setParameter": 1, progressMonitor: {deadline: -5}}));

// Setting parameter properly during runtime.
assert.commandWorked(st.s1.adminCommand(
    {"setParameter": 1, progressMonitor: {deadline: NumberInt(CUSTOM_DEADLINE + 1)}}));
result = st.s1.adminCommand({"getParameter": 1, "progressMonitor": 1});
assert.eq(result.progressMonitor.deadline, CUSTOM_DEADLINE + 1);
// Setting only one sub-field will reset others to their default.
assert.eq(result.progressMonitor.interval, 50);

assert.commandWorked(st.s1.adminCommand({
    "setParameter": 1,
    progressMonitor:
        {deadline: NumberInt(CUSTOM_DEADLINE + 1), interval: NumberInt(CUSTOM_INTERVAL)}
}));
result = st.s1.adminCommand({"getParameter": 1, "progressMonitor": 1});
assert.eq(result.progressMonitor.deadline, CUSTOM_DEADLINE + 1);
assert.eq(result.progressMonitor.interval, CUSTOM_INTERVAL);
st.stop();
}());
