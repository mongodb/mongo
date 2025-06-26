
/**
 * Tests behavior that is common to all rate limiters.
 * @tags: [requires_fcv_80]
 */

const testBurstCapacityIncrease = () => {
    const marginOfError = 5;  // +/- 5 tokens just in case there is rounding errors/clock skew
    const refreshRate = 10000;

    const initialBurstCapacitySecs = 0.5;
    const initialTargetTokens = refreshRate * initialBurstCapacitySecs;

    const newBurstCapacitySecs = 2;
    const newTargetTokens = refreshRate * newBurstCapacitySecs;

    const rateLimiterConfigurations = {
        sessionEstablishment: {
            startupServerParameters: {
                ingressConnectionEstablishmentRatePerSec: refreshRate,
                ingressConnectionEstablishmentBurstCapacitySecs: initialBurstCapacitySecs,
            },
            burstCapacityParameterName: "ingressConnectionEstablishmentBurstCapacitySecs",
            serverStatusSection: (conn) =>
                conn.adminCommand({serverStatus: 1}).queues.ingressSessionEstablishment
        },
        ingressRequest: {
            startupServerParameters: {
                ingressRequestAdmissionRatePerSec: refreshRate,
                ingressRequestAdmissionBurstCapacitySecs: initialBurstCapacitySecs,
                ingressRequestRateLimiterEnabled: true,
                featureFlagIngressRateLimiting: 1,
            },
            burstCapacityParameterName: "ingressRequestAdmissionBurstCapacitySecs",
            serverStatusSection: (conn) =>
                conn.adminCommand({serverStatus: 1}).network.ingressRequestRateLimiter
        }
    };

    for (const [name, rateLimiter] of Object.entries(rateLimiterConfigurations)) {
        jsTestLog("Testing burst capacity dynamic sizing for rate limiter: " + name);

        let mongo = MongoRunner.runMongod({setParameter: rateLimiter.startupServerParameters});

        let initialTokens = rateLimiter.serverStatusSection(mongo).totalAvailableTokens;
        assert.between(initialTargetTokens - marginOfError,
                       initialTokens,
                       initialTargetTokens + marginOfError);

        assert.commandWorked(mongo.adminCommand(
            {setParameter: 1, [rateLimiter.burstCapacityParameterName]: newBurstCapacitySecs}));

        assert.soon(() => {
            const newTokens = rateLimiter.serverStatusSection(mongo).totalAvailableTokens;
            jsTestLog("New tokens: " + newTokens);
            return newTargetTokens - marginOfError <= newTokens &&
                newTokens <= newTargetTokens + marginOfError;
        });

        MongoRunner.stopMongod(mongo);
    }
};

testBurstCapacityIncrease();