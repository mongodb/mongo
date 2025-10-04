// Hook to disable the failpoint that creates random interrupts on a particular thread. We disable
// the failpoint for the duration of the serverInfo section of the fuzzer's preamble.
import {defineFuzzerHooks} from "jstests/libs/jstestfuzz/hook_utils.js";

(function () {
    let threadName;

    const disableCheckForInterruptFailFP = function () {
        // There is no synchronization between fuzzer clients so this hook cannot run with the
        // concurrent fuzzer.
        assert.eq(
            TestData.numTestClients,
            1,
            "Cannot run the check for interrupt hook when there is more than 1 client",
        );

        const myUriRes = assert.commandWorked(db.runCommand({whatsmyuri: 1}));
        const myUri = myUriRes.you;

        const curOpRes = assert.commandWorked(db.adminCommand({currentOp: 1, client: myUri}));
        threadName = curOpRes.inprog[0].desc;

        assert.commandWorked(
            db.adminCommand({
                configureFailPoint: "checkForInterruptFail",
                mode: "off",
            }),
        );
    };

    const enableCheckForInterruptFailFP = function () {
        const chance = TestData.checkForInterruptFailpointChance;

        assert.gte(chance, 0, "checkForInterruptFailpointChance must be >= 0");
        assert.lte(chance, 1, "checkForInterruptFailpointChance must be <= 1");

        assert.commandWorked(
            db.adminCommand({
                configureFailPoint: "checkForInterruptFail",
                mode: "alwaysOn",
                data: {threadName, chance},
            }),
        );
    };

    defineFuzzerHooks({
        beforeServerInfo: disableCheckForInterruptFailFP,
        afterServerInfo: enableCheckForInterruptFailFP,
    });
})();
