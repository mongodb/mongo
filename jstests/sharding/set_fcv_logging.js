/**
 * Tests for logging during FCV upgrade/downgrade.
 * Three logs are involved:
 *      FCV upgrade or downgrade called     (6744300)
 *      FCV is upgrading or downgrading     (6744301)
 *      FCV upgrade or downgrade success    (6744302).
 *
 * @tags: [
 *   multiversion_incompatible,
 *   does_not_support_stepdowns,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const latest = "latest";
const SetFCVStatus = Object.freeze({called: 1, transitioning: 2, successful: 3});

/**
 * Helper function to assert logging information including FCV status and server type.
 *
 * 'isSymmetricFCV' reflects whether the SymmetricFCV feature flag is enabled on the cluster under
 * test; it changes how a resuming setFCV behaves (see 'isResume' below).
 *
 * 'isResume' must be set when the setFCV being asserted resumes a transition that a previous
 * (failpoint-interrupted) setFCV already moved into the transitional state. This matters under
 * Symmetric FCV: the setFCV protocol persists its phase in the FCV document and a resuming command
 * continues from that phase instead of restarting from the beginning. Because the "FCV is
 * transitioning" log (6744301) is only emitted when the transition first enters the transitional
 * state (the kStart phase), a Symmetric FCV resume does NOT re-emit it -- only the "called" (6744300)
 * and "success" (6744302) logs are produced. Without Symmetric FCV every setFCV restarts from kStart
 * and re-emits all logs, so 'isResume' has no effect.
 */
function assertLogs(
    status,
    upgradeOrDowngrade,
    serverType,
    numShardServers,
    isSymmetricFCV,
    isResume = false,
) {
    // A resume only makes sense for a transition that runs to completion: the resuming setFCV picks
    // up from the persisted transitional phase and drives it to success. Asserting a resume that
    // stopped at 'called' or 'transitioning' would expect per-shard logs that were never produced,
    // failing deep inside the shard-log count with a confusing message. Reject that combination up
    // front so the caller gets immediate, actionable feedback instead.
    assert(
        !isResume || status === SetFCVStatus.successful,
        "assertLogs: isResume=true is only valid with status=successful",
    );

    const skipsTransitioningLog = isSymmetricFCV && isResume;

    const expectCalled = status >= SetFCVStatus.called;
    const expectTransitioning = status >= SetFCVStatus.transitioning && !skipsTransitioningLog;
    const expectSuccessful = status >= SetFCVStatus.successful;

    if (expectCalled) {
        assert.soon(
            () => rawMongoProgramOutput('"id":6744300').match(/\"id\":6744300/),
            '"FCV ' + upgradeOrDowngrade + ' called" log not found',
        );
    } else {
        assert(
            rawMongoProgramOutput('"id":6744300').match(/\"id\":6744300/) == null,
            'should not log but "FCV ' + upgradeOrDowngrade + ' called" log found',
        );
    }
    if (expectTransitioning) {
        assert.soon(
            () => rawMongoProgramOutput('"id":6744301').match(/\"id\":6744301/),
            '"FCV ' + upgradeOrDowngrade + ' in progress" log not found',
        );
    } else {
        assert(
            rawMongoProgramOutput('"id":6744301').match(/\"id\":6744301/) == null,
            'should not log but "FCV ' + upgradeOrDowngrade + ' in progress" log found',
        );
    }
    if (expectSuccessful) {
        assert.soon(
            () => rawMongoProgramOutput('"id":6744302').match(/\"id\":6744302/),
            '"FCV ' + upgradeOrDowngrade + ' success" log not found',
        );
    } else {
        assert(
            rawMongoProgramOutput('"id":6744302').match(/\"id\":6744302/) == null,
            'should not log but "FCV ' + upgradeOrDowngrade + ' success" log found',
        );
    }

    // Number of FCV logs expected on the orchestrating node (the replica set primary or, in a
    // sharded cluster, the config server): one per log line that was emitted above.
    const numOrchestratorLogs =
        (expectCalled ? 1 : 0) + (expectTransitioning ? 1 : 0) + (expectSuccessful ? 1 : 0);

    if (serverType === "replica set/maintenance mode") {
        assert.soon(
            () => {
                let matchRes = rawMongoProgramOutput('"serverType"').match(
                    /\"serverType\":"replica set\/maintenance mode"/g,
                );
                return matchRes != null && matchRes.length == numOrchestratorLogs;
            },
            "should have " +
                numOrchestratorLogs +
                " log(s) with serverType: replica set/maintenance mode",
        );
    } else if (serverType === "shardedCluster") {
        assert.soon(
            () => {
                let matchRes = rawMongoProgramOutput('"serverType"').match(
                    /\"serverType\":"config server"/g,
                );
                return matchRes != null && matchRes.length == numOrchestratorLogs;
            },
            "should have " + numOrchestratorLogs + " log(s) with serverType: config server",
        );
        // Per-shard FCV logs. If the FCV change failed before the config server reached the
        // transitioning state, the shards were never contacted and there are no shard-server logs.
        // On a Symmetric FCV resume the config server picks up from the persisted phase and only
        // drives the shards through the final phases, so each shard emits a single "success" log
        // rather than the full called/transitioning/success sequence.
        let nExpectedLogs;
        if (skipsTransitioningLog) {
            nExpectedLogs = numShardServers * 1;
        } else {
            nExpectedLogs = status >= SetFCVStatus.transitioning ? numShardServers * status : 0;
        }

        if (nExpectedLogs > 0) {
            assert.soon(
                () => {
                    let matchRes = rawMongoProgramOutput('"serverType"').match(
                        /\"serverType\":\"shard server\"/g,
                    );
                    return matchRes != null && matchRes.length === nExpectedLogs;
                },
                "should have " + nExpectedLogs + " log(s) with serverType: shard server",
            );
        } else {
            let matchRes = rawMongoProgramOutput('"serverType"').match(
                /\"serverType\":"shard server"/g,
            );
            assert(matchRes == null, "should not have log containing shard server");
        }
    }
    clearRawMongoProgramOutput(); // Clears output for next logging.
}

/**
 * Test the correct logs are logged using failpoints:
 *
 * failBeforeTransitioning - set after FCV change is called and before setTargetVersion is set
 * to true, therefore should only log 6744300 (called).
 *
 * failDowngrading/failUpgrading - set after setTargetVersion is true, inside the
 * _runDowngrade/_runUpgrade function, therefore should only log 6744300 (called),
 * 6744301 (transitioning).
 *
 * Each server type is tested with upgrade and downgrade with the two mentioned failpoints on/off.
 */
function assertLogsWithFailpoints(conn, adminDB, serverType, numShardServers) {
    const isSymmetricFCV = FeatureFlagUtil.isEnabled(adminDB, "SymmetricFCV");

    clearRawMongoProgramOutput(); // Clears output for next logging.

    /* 1. Testing logging for downgrade */

    // 1.1. Test that setFCV (downgrade) fails before moving to the transitional FCV:
    // should have log 6744300 (called), but not 6744301 (transitioning) or 6744302 (successful).
    jsTest.log("Case 1.1. Logs should show that downgrade has only reached 'called' stage.");
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "failBeforeTransitioning", mode: "alwaysOn"}),
    );
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    assertLogs(SetFCVStatus.called, "downgrade", serverType, numShardServers, isSymmetricFCV);
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "failBeforeTransitioning", mode: "off"}),
    );

    // 1.2. Test that setFCV (downgrade) fails before finishing downgrading:
    // should have log 6744300 (called), 6744301 (transitioning), but not 6744302 (successful).
    jsTest.log("Case 1.2. Logs should show that downgrade has only reached 'downgrading' stage.");
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "failDowngrading", mode: "alwaysOn"}),
    );
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    assertLogs(
        SetFCVStatus.transitioning,
        "downgrade",
        serverType,
        numShardServers,
        isSymmetricFCV,
    );
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failDowngrading", mode: "off"}));

    // 1.3. Test that setFCV (downgrade) succeeds:
    // should have all three logs 6744300 (called), 6744301 (transitioning), 6744302 (successful).
    jsTest.log("Case 1.3. Logs should show that downgrade has reached 'downgraded' stage.");
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
    );
    // This setFCV resumes the downgrade interrupted by the 'failDowngrading' failpoint in case 1.2.
    assertLogs(
        SetFCVStatus.successful,
        "downgrade",
        serverType,
        numShardServers,
        isSymmetricFCV,
        true /* isResume */,
    );

    /* 2. Testing logging for upgrade */

    // 2.1. Test that setFCV (upgrade) fails before moving to the transitional FCV:
    // should have log 6744300 (called), but not 6744301 (transitioning) or 6744302 (successful).
    jsTest.log("Case 2.1. Logs should show that upgrade has only reached 'called' stage.");
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "failBeforeTransitioning", mode: "alwaysOn"}),
    );
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );
    assertLogs(SetFCVStatus.called, "upgrade", serverType, numShardServers, isSymmetricFCV);
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "failBeforeTransitioning", mode: "off"}),
    );

    // 2.2. Test that setFCV (upgrade) fails before finishing upgrading:
    // should have log 6744300 (called), 6744301 (transitioning), but not 6744302 (successful).
    jsTest.log("Case 2.2. Logs should show that upgrade has only reached 'upgrading' stage.");
    assert.commandWorked(
        conn.adminCommand({configureFailPoint: "failUpgrading", mode: "alwaysOn"}),
    );
    assert.commandFailed(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );
    assertLogs(SetFCVStatus.transitioning, "upgrade", serverType, numShardServers, isSymmetricFCV);
    assert.commandWorked(conn.adminCommand({configureFailPoint: "failUpgrading", mode: "off"}));

    // 2.3. Test that setFCV (upgrade) succeeds:
    // should have all three logs 6744300 (called), 6744301 (transitioning), 6744302 (successful).
    jsTest.log("Case 2.3. Logs should show that upgrade has reached 'upgraded' stage.");
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );
    // This setFCV resumes the upgrade interrupted by the 'failUpgrading' failpoint in case 2.2.
    assertLogs(
        SetFCVStatus.successful,
        "upgrade",
        serverType,
        numShardServers,
        isSymmetricFCV,
        true /* isResume */,
    );

    // 2.4. Shouldn't log anything because we have already upgraded to latestFCV.
    jsTest.log(
        "Case 2.4. Logs should not contain fcv upgrade because it is already in upgraded state.",
    );
    assert.commandWorked(
        adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
    );
    // Ensure none of the logs have been output.
    assertLogs(
        0 /* status: no fcv change */,
        "upgrade",
        0 /* serverType */,
        0 /* numShardServers */,
        isSymmetricFCV,
    );
}

function runStandaloneTest() {
    // A 'latest' binary standalone should default to 'latestFCV'.
    const conn = MongoRunner.runMongod({binVersion: latest});
    assert.neq(
        null,
        conn,
        "mongod was unable to start up with version=" + latest + " and no data files",
    );
    const adminDB = conn.getDB("admin");
    checkFCV(adminDB, latestFCV);

    jsTest.log("Checking for correct FCV logging on a standalone.");
    assertLogsWithFailpoints(conn, adminDB, "replica set/maintenance mode", 0 /*numShardServers*/);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    const rst = new ReplSetTest({nodes: 2, nodeOptions: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    const primary = rst.getPrimary();
    const primaryAdminDB = primary.getDB("admin");
    checkFCV(primaryAdminDB, latestFCV);

    jsTest.log("Checking for correct FCV logging on a replica set.");
    assertLogsWithFailpoints(
        primary,
        primaryAdminDB,
        "replica set/maintenance mode",
        0 /*numShardServers*/,
    );

    rst.stopSet();
}

function runShardingTest() {
    const st = new ShardingTest({
        mongos: 1,
        config: 1,
        shards: 2,
        other: {
            mongosOptions: {binVersion: latest},
            configOptions: {binVersion: latest},
            rsOptions: {
                binVersion: latest,
            },
            rs: {nodes: 2},
        },
    });
    const mongosAdminDB = st.s.getDB("admin");
    const configPrimaryAdminDB = st.configRS.getPrimary().getDB("admin");
    const shardPrimaryAdminDB = st.rs0.getPrimary().getDB("admin");
    checkFCV(configPrimaryAdminDB, latestFCV);
    checkFCV(shardPrimaryAdminDB, latestFCV);

    jsTest.log("Checking for correct FCV logging on a sharded cluster.");
    // One of the shards is the config server in config shard mode.
    const numShardServers = TestData.configShard ? 1 : 2;
    assertLogsWithFailpoints(
        st.configRS.getPrimary(),
        mongosAdminDB,
        "shardedCluster",
        numShardServers,
    );

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTest();
