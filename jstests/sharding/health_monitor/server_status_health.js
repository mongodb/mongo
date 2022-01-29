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
assert(result.testObserver.intensity);

const faultInformation = result.faultInformation;
// TODO: SERVER-60973 check fault severity
assert(faultInformation.duration);
assert(faultInformation.facets);
assert.eq(faultInformation.numFacets, 1);
assert(faultInformation.facets.testObserver);

const kTestObserverFacet = faultInformation.facets.testObserver;
assert.eq(kTestObserverFacet.duration, faultInformation.duration);
assert(kTestObserverFacet.description.includes("InternalError: test msg"));

// Check server status after test health observer enabled and failpoint returns success.
assert.commandWorked(
    st.s0.adminCommand({"configureFailPoint": 'testHealthObserver', "mode": "alwaysOn"}));

assert.soon(() => {
    result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
    return result.state == "Ok";
});

print("---RESULT 3---");
result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
print(tojson(result));
assert.eq(result.state, "Ok");
assert(result.enteredStateAtTime);

print("---RESULT 4 with details---");
result =
    assert.commandWorked(st.s0.adminCommand({serverStatus: 1, health: {details: true}})).health;
print(tojson(result));
const testObserver = result.testObserver;
assert(testObserver.totalChecks);
assert(testObserver.totalChecks >= 1);
assert(testObserver.totalChecksWithFailure >= 1);
assert(testObserver.timeSinceLastCheckStartedMs >= 1);
assert(testObserver.timeSinceLastCheckCompletedMs >= 1);

st.stop();
})();
