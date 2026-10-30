/* Ensure that during a stepDown operation we fassert when
 * getting the RSTL takes longer than fassertOnLockTimeoutForStepDownOverride seconds.
 *
 * 1. Start up a 3 node replica set.
 * 2. Enable failpoint hangWithLockDuringBatchInsert.
 * 3. Do one write in the background that, due to the failpoint, will hang
 *    after getting locks. If featureFlagIntentRegistration disabled, includes RSTL;
 *    if enabled, the write holds a write intent instead.
 * 4. Send in the background request to the PRIMARY to StepDown.
 *    stepDown thread will time out since it can't get the RSTL(featureFlagIntentRegistration
 *    disabled) or drain the write intent(featureFlagIntentRegistration enabled) and
 *    cause an fassert (node crash) on the primary [since deadline is beyond the fassert timeout].
 * 5. Wait for the primary to go DOWN.
 * 6. Verify from the primary's log timestamps that the fassert fired within the timeframe imposed
 *    by fassertOnLockTimeoutForStepDownOverride after the stepdown started, and that it came from
 *    the expected code path for the featureFlagIntentRegistration setting.
 *
 * @tags: [
 *     requires_majority_read_concern,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {afterEach, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// "Attempting to step down in response to replSetStepDown command".
const kStepDownStartedLogId = 21579;

// Returns the time in milliseconds of the first log line with the given id from the node on `port`.
function getLogTimeMs(port, logId) {
    const line = rawMongoProgramOutput(`d${port}\\| .*"id":${logId},`).split("\n")[0];
    assert(line, "Expected the node to log the given id", {port, logId});
    return Date.parse(JSON.parse(line.slice(line.indexOf("{"))).t.$date);
}

describe("stepDown fasserts after the explicitly overridden RSTL timeout", function () {
    const name = "interruptStepDown";
    let replSet = null;

    before(function () {
        // Because this test intentionally crashes the server via an fassert, we need to instruct
        // the shell to clean up the core dump that is left behind.
        TestData.cleanUpCoreDumpsFromExpectedCrash = true;
    });

    afterEach(function () {
        if (replSet) {
            replSet.stopSet(undefined, undefined, {skipValidation: true});
            replSet = null;
        }
    });

    // TODO SERVER-106669: Remove the false case when featureFlagIntentRegistration is removed.
    const cases = [
        // The write holds a write intent, so the stepdown times out draining intents.
        {intentRegistrationEnabled: true, expectedLogId: 9795404},
        // The write holds the RSTL, so the stepdown times out acquiring the RSTL.
        {intentRegistrationEnabled: false, expectedLogId: 5675600},
    ];

    for (const {intentRegistrationEnabled, expectedLogId} of cases) {
        it(`featureFlagIntentRegistration=${intentRegistrationEnabled}`, function () {
            clearRawMongoProgramOutput();

            replSet = new ReplSetTest({name: name, nodes: 3});
            const nodes = replSet.nodeList();
            replSet.startSet({
                setParameter: {featureFlagIntentRegistration: intentRegistrationEnabled},
            });
            replSet.initiate(
                {
                    "_id": name,
                    "members": [
                        {"_id": 0, "host": nodes[0]},
                        {"_id": 1, "host": nodes[1]},
                        {"_id": 2, "host": nodes[2], "priority": 0},
                    ],
                },
                null,
                {initiateWithDefaultElectionTimeout: true},
            );

            const primary = replSet.getPrimary();
            assert.commandWorked(
                primary.adminCommand({
                    setDefaultRWConcern: 1,
                    defaultWriteConcern: {w: 1},
                    writeConcern: {w: "majority"},
                }),
            );
            replSet.awaitReplication();

            // Makes sure writes work
            assert.commandWorked(primary.getDB(name).foo.insert([{_id: 0}, {_id: 1}, {_id: 2}]));
            replSet.awaitReplication();

            // Enable failpoint which waits after getting locks
            const failPoint = configureFailPoint(
                primary.getDB(name),
                "hangWithLockDuringBatchInsert",
                {
                    shouldCheckForInterrupt: false,
                    shouldContinueOnInterrupt: false,
                },
            );

            jsTestLog("Initiating write which will hang on failpoint");
            const bgInserter = startParallelShell(
                "db.getSiblingDB('interruptStepDown').foo.insert([{_id:0}, {_id:1}, {_id:2}]);",
                primary.port,
            );

            jsTestLog("Wait for failpoint after bgwrite and before asking the PRIMARY to stepdown");
            failPoint.wait();

            const stepDownCmd = function () {
                jsTestLog("Sending stepdown to primary");
                db.getSiblingDB("admin").runCommand({
                    replSetStepDown: 20,
                    force: true,
                    fassertOnLockTimeoutForStepDownOverride: 10,
                });
            };

            const stepDowner = startParallelShell(stepDownCmd, primary.port);

            jsTestLog("Waiting for primary to be down");
            replSet.waitForState(primary, ReplSetTest.State.DOWN);

            // Measure when the fassert fired from the log timestamps. The override is
            // 10 seconds and the default fassertOnLockTimeoutForStepUpDown is 60 seconds, so an
            // upper bound of 30 seconds proves the override shortened the timeout.
            // The fatal log id shows the crash came from the code path that
            // featureFlagIntentRegistration selects.
            const fassertDelaySecs =
                (getLogTimeMs(primary.port, expectedLogId) -
                    getLogTimeMs(primary.port, kStepDownStartedLogId)) /
                1000;
            jsTest.log.info("Primary fasserted after the stepdown started", {fassertDelaySecs});
            assert(
                fassertDelaySecs >= 9 && fassertDelaySecs < 30,
                "fassert did not fire at the overridden timeout",
                {fassertDelaySecs, expectedLogId, intentRegistrationEnabled},
            );

            // Both parallel shells were started against the original primary and are expected to
            // exit non-zero once that node aborts during stepdown.
            stepDowner({checkExitSuccess: false});
            bgInserter({checkExitSuccess: false});

            // We expect primary to have crashed with an fassert.
            replSet.stop(primary.nodeId, undefined, {
                forRestart: false,
                allowedExitCode: MongoRunner.EXIT_ABORT,
            });
            replSet.stopSet();
            replSet = null;
        });
    }
});
