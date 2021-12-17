/**
 * Turning off health observer during transient fault removes the associated fault facet and
 * transitions back to Ok.
 *
 *  @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "test", intensity: "off"},
                {type: "ldap", intensity: "off"},
                {type: "dns", intensity: "off"}
            ]
        }),
        featureFlagHealthMonitoring: true,
        logComponentVerbosity: tojson({processHealth: {verbosity: 4}})
    }
};

let st = new ShardingTest({
    mongos: [params],
    shards: 1,
});

function healthStatus() {
    return assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
}

function waitForState(state) {
    assert.soon(() => {
        let result = healthStatus();
        jsTestLog(tojson(result));
        return result.state === state;
    });
}

function changeObserverIntensity(observer, intensity) {
    let paramValue = {"values": [{"type": observer, "intensity": intensity}]};
    assert.commandWorked(
        st.s0.adminCommand({"setParameter": 1, healthMonitoringIntensities: paramValue}));
}

jsTestLog("Wait for initial health checks to complete.");
waitForState("Ok");

jsTestLog("Test observer signals fault");
assert.commandWorked(st.s0.adminCommand({
    "configureFailPoint": 'testHealthObserver',
    "data": {"code": "InternalError", "msg": "test msg"},
    "mode": "alwaysOn"
}));
changeObserverIntensity("test", "critical");

waitForState("TransientFault");

jsTestLog("Turn off observer during transient fault");
changeObserverIntensity("test", "off");

waitForState("Ok");

jsTestLog("Turn on observer after fault resolution");
changeObserverIntensity("test", "critical");
waitForState("TransientFault");

jsTestLog("Test was successful");

st.stop();
})();
