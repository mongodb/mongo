/*
 *  @tags: [multiversion_incompatible]
 */

(function() {
'use strict';

let CUSTOM_INTERVAL = 1337;
let CUSTOM_DEADLINE = 5;

var st = new ShardingTest({
    mongos: [
        {
            setParameter: {
                healthMonitoringIntensities: tojson({
                    values: [
                        {type: "dns", intensity: "off"},
                        {type: "ldap", intensity: "off"},
                        {type: "test", intensity: "critical"}
                    ]
                }),
            }
        },
        {
            setParameter: {
                healthMonitoringIntensities: tojson({
                    values: [
                        {type: "dns", intensity: "off"},
                        {type: "ldap", intensity: "off"},
                        {type: "test", intensity: "off"}
                    ]
                }),
                progressMonitor: tojson({interval: CUSTOM_INTERVAL, deadline: CUSTOM_DEADLINE}),
                healthMonitoringIntervals:
                    tojson({values: [{type: "test", interval: CUSTOM_INTERVAL}]})
            }
        }
    ],
    shards: 1,
});

// Intensity parameter
let result = st.s0.adminCommand({"getParameter": 1, "healthMonitoringIntensities": 1});
let getIntensity = (result, typeOfObserver) => {
    let intensities = result.healthMonitoringIntensities.values;
    let foundPair = intensities.find(({type}) => type === typeOfObserver);
    if (foundPair) {
        return foundPair.intensity;
    }
};

assert.eq(getIntensity(result, "dns"), "off");
assert.eq(getIntensity(result, "ldap"), "off");
assert.eq(getIntensity(result, "test"), "critical");

assert.commandWorked(st.s0.adminCommand({
    "setParameter": 1,
    healthMonitoringIntensities: {values: [{type: "test", intensity: "non-critical"}]}
}));
assert.commandFailed(st.s0.adminCommand({
    "setParameter": 1,
    healthMonitoringIntensities: {values: [{type: "dns", intensity: "INVALID"}]}
}));
assert.commandFailed(st.s0.adminCommand({
    "setParameter": 1,
    healthMonitoringIntensities: {values: [{type: "invalid", intensity: "off"}]}
}));

// Tests that test param is unchanged after dns was changed.
result =
    assert.commandWorked(st.s0.adminCommand({"getParameter": 1, healthMonitoringIntensities: 1}));
assert.eq(getIntensity(result, "dns"), "off");
assert.eq(getIntensity(result, "test"), "non-critical");

assert.commandWorked(st.s0.adminCommand({
    "setParameter": 1,
    healthMonitoringIntensities:
        {values: [{type: "dns", intensity: 'non-critical'}, {type: "test", intensity: 'off'}]}
}));
result =
    assert.commandWorked(st.s0.adminCommand({"getParameter": 1, healthMonitoringIntensities: 1}));

assert.eq(getIntensity(result, "dns"), "non-critical");
assert.eq(getIntensity(result, "ldap"), "off");

// Interval parameter
let getInterval = (commandResult, typeOfObserver) => {
    let allValues = commandResult.healthMonitoringIntervals.values;
    let foundPair = allValues.find(({type}) => type === typeOfObserver);
    if (foundPair) {
        return foundPair.interval;
    }
};

result = st.s1.adminCommand({"getParameter": 1, "healthMonitoringIntervals": 1});
assert.eq(getInterval(result, "test"), CUSTOM_INTERVAL);

assert.commandWorked(st.s1.adminCommand({
    "setParameter": 1,
    healthMonitoringIntervals: {values: [{type: "dns", interval: NumberInt(100)}]}
}));
assert.commandFailed(st.s1.adminCommand({
    "setParameter": 1,
    healthMonitoringIntervals: {values: [{type: "dns", interval: NumberInt(0)}]}
}));
assert.commandFailed(st.s1.adminCommand({
    "setParameter": 1,
    healthMonitoringIntervals: {values: [{type: "invalid", interval: NumberInt(100)}]}
}));

// Tests that test param is unchanged, dns is set to 100.
result =
    assert.commandWorked(st.s1.adminCommand({"getParameter": 1, healthMonitoringIntervals: 1}));
assert.eq(getInterval(result, "test"), CUSTOM_INTERVAL);
assert.eq(getInterval(result, "dns"), 100);

assert.commandWorked(st.s1.adminCommand({
    "setParameter": 1,
    healthMonitoringIntervals: {
        values:
            [{type: "dns", interval: NumberInt(2000)}, {type: "ldap", interval: NumberInt(600000)}]
    }
}));

result =
    assert.commandWorked(st.s1.adminCommand({"getParameter": 1, healthMonitoringIntervals: 1}));
assert.eq(getInterval(result, "dns"), 2000);
assert.eq(getInterval(result, "ldap"), 600000);

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
assert.eq(result.progressMonitor.interval, 1000);

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
