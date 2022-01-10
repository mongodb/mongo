/**
 * Tests behavior of non-critical fault facet.
 *
 *  @tags: [multiversion_incompatible]
 */
(function() {
'use strict';
const ACTIVE_FAULT_DURATION_SECS = 1;

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "test", intensity: "non-critical"},
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

assert.commandWorked(
    st.s0.adminCommand({"setParameter": 1, activeFaultDurationSecs: ACTIVE_FAULT_DURATION_SECS}));

let result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
assert.eq(result.state, "Ok");

// Failpoint returns fault.
assert.commandWorked(st.s0.adminCommand({
    "configureFailPoint": 'testHealthObserver',
    "data": {"code": "InternalError", "msg": "test msg"},
    "mode": "alwaysOn"
}));

assert.soon(() => {
    result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
    return result.state == "TransientFault";
});

// Sleep for twice as long as active fault duration (in Millis).
sleep(ACTIVE_FAULT_DURATION_SECS * 2000);

// Still in transient fault.
result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
assert.eq(result.state, "TransientFault");
assert(result.faultInformation.facets.testObserver.description.includes("InternalError: test msg"));

st.stop();
})();
