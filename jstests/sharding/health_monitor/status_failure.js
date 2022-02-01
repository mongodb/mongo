/**
 * Tests behavior of fault manager when health observer returns status failure.
 *
 *  @tags: [multiversion_incompatible]
 */
(function() {
'use strict';

const kWaitForCompletedChecksCount = 12;
const kWaitForPassedChecksCount = 8;

const params = {
    setParameter: {
        healthMonitoringIntensities: tojson({
            values: [
                {type: "test", intensity: "critical"},
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

let result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
assert.eq(result.state, "Ok");

const checkServerStats = function() {
    while (true) {
        // Failpoint returns status failure.
        assert.commandWorked(st.s0.adminCommand(
            {"configureFailPoint": 'statusFailureTestHealthObserver', "mode": "alwaysOn"}));

        assert.soon(() => {
            result = assert.commandWorked(st.s0.adminCommand({serverStatus: 1})).health;
            return result.state == "TransientFault" &&
                result.faultInformation.facets.testObserver.description.includes(
                    "Status failure in test health observer");
        });

        assert.commandWorked(st.s0.adminCommand(
            {"configureFailPoint": 'statusFailureTestHealthObserver', "mode": "off"}));

        assert.soon(() => {
            result =
                assert.commandWorked(st.s0.adminCommand({serverStatus: 1, health: {details: true}}))
                    .health;
            return result.state == "Ok";
        });

        print(`Server status: ${tojson(result)}`);
        // Wait for: at least kWaitForPassedChecksCount checks completed.
        if (result.testObserver.totalChecks >= kWaitForCompletedChecksCount &&
            result.testObserver.totalChecks - result.testObserver.totalChecksWithFailure >=
                kWaitForPassedChecksCount) {
            break;
        }
        sleep(1000);
    }
};

checkServerStats();

st.stop();
})();
