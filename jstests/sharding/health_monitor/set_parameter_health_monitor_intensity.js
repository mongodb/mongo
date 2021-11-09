

(function() {
'use strict';

var st = new ShardingTest({
    mongos: [{
        setParameter: {
            healthMonitoring: tojson({dns: "off", ldap: "critical"}),
        }
    }],
    shards: 1,
});

var result = st.s0.adminCommand({"getParameter": 1, "healthMonitoring": 1});
print(tojson(result));
assert.eq(result.healthMonitoring.dns, "off");
assert.eq(result.healthMonitoring.ldap, "critical");

assert.commandFailed(st.s0.adminCommand({"setParameter": 1, healthMonitoring: {dns: "INVALID"}}));
assert.commandFailed(st.s0.adminCommand({"setParameter": 1, healthMonitoring: {invalid: "off"}}));

assert.commandWorked(
    st.s0.adminCommand({"setParameter": 1, healthMonitoring: {dns: 'non-critical', ldap: 'off'}}));
var result = assert.commandWorked(st.adminCommand({"getParameter": 1, healthMonitoring: 1}));
print(tojson(result));
assert.eq(result.healthMonitoring.dns, "non-critical");
assert.eq(result.healthMonitoring.ldap, "off");
}());
