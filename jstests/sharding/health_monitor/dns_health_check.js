/**
 * Tests successful dns health check.
 *
 *  @tags: [
 *    multiversion_incompatible,
 *    # TODO (SERVER-88126): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kWaitForCompletedChecksCount = 30;
const kWaitForPassedChecksCount = 10;
const kMonitoringIntervalMs = 200;

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({values: [{type: "dns", intensity: "critical"}]}),
        healthMonitoringIntervals:
            tojson({values: [{type: "dns", interval: kMonitoringIntervalMs}]})
    }
};

let st = new ShardingTest({
    mongos: [params],
    shards: 1,
});

const checkServerStats = function() {
    while (true) {
        let result =
            assert.commandWorked(st.s0.adminCommand({serverStatus: 1, health: {details: true}}))
                .health;
        print(`Server status: ${tojson(result)}`);
        // Wait for: at least kWaitForPassedChecksCount checks completed.
        // At least some checks passed (more than 1).
        if (result.DNS.totalChecks >= kWaitForCompletedChecksCount &&
            result.DNS.totalChecks - result.DNS.totalChecksWithFailure >=
                kWaitForPassedChecksCount) {
            break;
        }
        sleep(1000);
    }
};

checkServerStats();

// Failpoint returns bad hostname.
assert.commandWorked(st.s0.adminCommand({
    "configureFailPoint": 'dnsHealthObserverFp',
    "data": {"hostname": "name.invalid"},
    "mode": "alwaysOn"
}));

let result;

assert.soon(() => {
    result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
    return result.state == "TransientFault";
});

// Failpoint off
assert.commandWorked(
    st.s0.adminCommand({"configureFailPoint": 'dnsHealthObserverFp', "mode": "off"}));

assert.soon(() => {
    result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
    return result.state == "Ok";
});

st.stop();
