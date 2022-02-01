/*
 *  @tags: [requires_fcv_53]
 */

(function() {
'use strict';
const params = {
    healthMonitoringIntensities: tojson({
        values: [
            {type: "test", intensity: "critical"},
        ]
    }),
};
const setFailPoint = {
    'failpoint.badConfigTestHealthObserver': "{'mode':'alwaysOn'}"
};

let st = new ShardingTest({
    mongos: [
        {setParameter: params},
        {setParameter: Object.assign({}, params, setFailPoint), waitForConnect: false}
    ]
});
assert.commandWorked(st.s0.adminCommand({"ping": 1}));  // Ensure s0 is unaffected.
try {
    new Mongo("127.0.0.1:" + st.s1.port);
    assert(false);
} catch (e) {
    assert(
        tojson(e).indexOf("network error") >= 0 || tojson(e).indexOf("couldn't connect to server"),
        "connection should fail with network error: " + tojson(e));
}
st.stop({skipValidatingExitCode: true});

// Verify that configuration doesn't matter when a health observer is off.
const offParams = {
    healthMonitoringIntensities: tojson({
        values: [
            {type: "test", intensity: "off"},
        ]
    })
};
st = new ShardingTest({mongos: [{setParameter: Object.assign({}, offParams, setFailPoint)}]});
assert.commandWorked(st.s0.adminCommand({"ping": 1}));
st.stop();
})();
