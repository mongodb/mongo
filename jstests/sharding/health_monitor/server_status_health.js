/**
 * Tests server status has correct fault/facet information.
 *
 *  @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

function changeObserverIntensity(observer, intensity) {
    let paramValue = {"values": [{"type": observer, "intensity": intensity}]};
    assert.commandWorked(
        st.s0.adminCommand({"setParameter": 1, healthMonitoringIntensities: paramValue}));
}

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "test", intensity: "off"},
                {type: "ldap", intensity: "off"},
                {type: "dns", intensity: "off"}
            ]
        }),
        featureFlagHealthMonitoring: true
    }
};

let st = new ShardingTest({
    mongos: [params],
    shards: 1,
    other: {mongosOptions: {setParameter: {featureFlagHealthMonitoring: true}}}
});

// Check server status after initial health check is complete.
let result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
print("---RESULT 1---");
print(tojson(result));
assert.eq(result.state, "Ok");
assert(result.enteredStateAtTime);

changeObserverIntensity('test', 'critical');

// Check server status after test health observer enabled and failpoint returns fault.
assert.commandWorked(st.s0.adminCommand({
    "configureFailPoint": 'testHealthObserver',
    "data": {"code": "InternalError", "msg": "test msg"},
    "mode": "alwaysOn"
}));

assert.soon(() => {
    result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
    return result.state == "TransientFault";
});

print("---RESULT 2---");
print(tojson(result));
assert(result.enteredStateAtTime);
assert(result.faultInformation);

const faultInformation = result.faultInformation;
assert.eq(faultInformation.severity, 1);
assert(faultInformation.duration);
assert(faultInformation.facets);
assert.eq(faultInformation.numFacets, 1);
assert(faultInformation.facets.kTestObserver);

const kTestObserverFacet = faultInformation.facets.kTestObserver;
assert.eq(kTestObserverFacet.severity, faultInformation.severity);
assert.eq(kTestObserverFacet.duration, faultInformation.duration);
assert(kTestObserverFacet.description.includes("InternalError: test msg"));

// Check server status after test health observer enabled and failpoint returns success.
assert.commandWorked(
    st.s0.adminCommand({"configureFailPoint": 'testHealthObserver', "mode": "alwaysOn"}));

assert.soon(() => {
    result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
    return result.state == "Ok";
});

result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
print("---RESULT 3---");
print(tojson(result));
assert.eq(result.state, "Ok");
assert(result.enteredStateAtTime);

st.stop();
})();
