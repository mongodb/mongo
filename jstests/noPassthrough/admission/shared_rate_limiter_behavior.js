
/**
 * Tests behavior that is common to all rate limiters.
 * @tags: [requires_fcv_80]
 */

const testBurstCapacityIncrease = () => {
    const marginOfError = 5;  // +/- 5 tokens just in case there is rounding errors/clock skew
    const initialRatePerSec = 10000;
    const initialBurstCapacitySecs = 0.5;
    const initialTargetTokens = initialRatePerSec * initialBurstCapacitySecs;

    const burstCapacityTest = (parameterName) => {
        const newBurstCapacitySecs = 2;
        return {
            parameterName,
            newValue: newBurstCapacitySecs,
            newTargetTokens: initialRatePerSec * newBurstCapacitySecs
        };
    };

    const ratePerSecTest = (parameterName) => {
        const newRatePerSec = 15_000;
        return {
            parameterName,
            newValue: newRatePerSec,
            newTargetTokens: newRatePerSec * initialBurstCapacitySecs,
        };
    };

    const rateLimiterConfigurations = {
        sessionEstablishment: {
            startupServerParameters: {
                ingressConnectionEstablishmentRatePerSec: initialRatePerSec,
                ingressConnectionEstablishmentBurstCapacitySecs: initialBurstCapacitySecs,
            },
            serverStatusSection: (conn) =>
                conn.adminCommand({serverStatus: 1}).queues.ingressSessionEstablishment,
            tests: [
                burstCapacityTest("ingressConnectionEstablishmentBurstCapacitySecs"),
                ratePerSecTest("ingressConnectionEstablishmentRatePerSec"),
            ],
        },
        ingressRequest: {
            startupServerParameters: {
                ingressRequestAdmissionRatePerSec: initialRatePerSec,
                ingressRequestAdmissionBurstCapacitySecs: initialBurstCapacitySecs,
                ingressRequestRateLimiterEnabled: true,
            },
            serverStatusSection: (conn) =>
                conn.adminCommand({serverStatus: 1}).network.ingressRequestRateLimiter,
            tests: [
                burstCapacityTest("ingressRequestAdmissionBurstCapacitySecs"),
                ratePerSecTest("ingressRequestAdmissionRatePerSec"),
            ],
        }
    };

    for (const [name, rateLimiter] of Object.entries(rateLimiterConfigurations)) {
        jsTestLog("Testing burst capacity dynamic sizing for rate limiter: " + name);

        let mongo = MongoRunner.runMongod({setParameter: {featureFlagIngressRateLimiting: true}});

        for (const test of rateLimiter.tests) {
            assert.commandWorked(
                mongo.adminCommand({setParameter: 1, ...rateLimiter.startupServerParameters}));

            let initialTokens = rateLimiter.serverStatusSection(mongo).totalAvailableTokens;
            assert.between(initialTargetTokens - marginOfError,
                           initialTokens,
                           initialTargetTokens + marginOfError);

            jsTestLog("Setting " + test.parameterName + " to " + test.newValue);
            assert.commandWorked(
                mongo.adminCommand({setParameter: 1, [test.parameterName]: test.newValue}));

            assert.soon(() => {
                const newTokens = rateLimiter.serverStatusSection(mongo).totalAvailableTokens;
                jsTestLog("New tokens: " + newTokens);
                return test.newTargetTokens - marginOfError <= newTokens &&
                    newTokens <= test.newTargetTokens + marginOfError;
            });
        }

        MongoRunner.stopMongod(mongo);
    }
};

testBurstCapacityIncrease();
