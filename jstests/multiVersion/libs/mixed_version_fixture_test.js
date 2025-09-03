import "jstests/multiVersion/libs/multi_rs.js";

import {copyJSON} from "jstests/libs/json_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const latestVersion = {
    binVersion: "latest",
};
const lastLTSVersion = {
    binVersion: "last-lts",
};

/**
 * Simulates a rolling restart of a Replica Set and calls assertion callbacks at each step along
 * the way. The procedure works like this:
 *   1. Spin up a ReplSet with startingVersion and startingNodeOptions.
 *   2. Call the setupFn() and beforeRestart() assertions.
 *   3. Restart the secondaries (with restartVersion and restartNodeOptions).
 *   4. Call the afterSecondariesHaveRestarted() assertions.
 *   5. Restart the primaries (with restartVersion and restartNodeOptions).
 *   6. Call the afterPrimariesHaveRestarted() assertions.
 *   7. [if afterFCVBump is non-null] Upgrade the FCV.
 *   8. [if afterFCVBump is non-null] Call the afterFCVBump() assertions.
 *   9. [if afterFCVBump is non-null] Downgrade the FCV.
 *   10. [if afterFCVBump is non-null] Call the afterPrimariesHaveRestarted() assertions again.
 */
export function testPerformReplSetRollingRestart({
    startingVersion = latestVersion,
    restartVersion = latestVersion,
    startingNodeOptions = {},
    restartNodeOptions = {},
    setupFn,
    beforeRestart,
    afterSecondariesHaveRestarted,
    afterPrimariesHaveRestarted,
    afterFCVBump = null,
}) {
    jsTest.log.info(
        "Starting a replica set with all nodes on version " +
            tojsononeline(startingVersion) +
            " with options " +
            tojsononeline(startingNodeOptions),
    );
    const rst = new ReplSetTest({
        name: jsTestName(),
        nodes: [
            {
                ...startingVersion,
            },
            {
                ...startingVersion,
            },
        ],
        // Create a copy of the options since the callee may modify them.
        nodeOptions: copyJSON(startingNodeOptions),
    });

    rst.startSet();
    rst.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

    let primaryConnection = rst.getPrimary();

    jsTest.log.info("Calling the setup function");
    setupFn(primaryConnection);

    jsTest.log.info("Calling the beforeRestart function");
    beforeRestart(primaryConnection);

    // Restart the secondaries only.
    // Create a copy of the options since the callee may modify them.
    jsTest.log.info(
        "Restarting the secondaries with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    rst.upgradeSecondaries({...restartVersion, ...copyJSON(restartNodeOptions)});
    primaryConnection = rst.getPrimary();

    jsTest.log.info("Calling the afterSecondariesHaveRestarted function");
    afterSecondariesHaveRestarted(primaryConnection);

    // TODO SERVER-109457 Try a scenario where you force election here so that the restarted
    // secondary could become primary while another active secondary has not restarted yet. This
    // will probably require adding a third node.

    // Restart the primary.
    // Create a copy of the options since the callee may modify them.
    jsTest.log.info(
        "Restarting the primary with version " +
            tojsononeline(restartVersion) +
            " and options " +
            tojsononeline(restartNodeOptions),
    );
    rst.upgradeSet({...restartVersion, ...copyJSON(restartNodeOptions)});
    primaryConnection = rst.getPrimary();

    jsTest.log.info("Calling the afterPrimariesHaveRestarted function");
    afterPrimariesHaveRestarted(primaryConnection);

    if (afterFCVBump) {
        const getAdminDB = () => primaryConnection.getDB("admin");

        // Upgrade the FCV.
        jsTest.log.info("Upgrading the FCV to " + tojsononeline(latestFCV));
        assert.commandWorked(getAdminDB().runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

        jsTest.log.info("Calling the afterFCVBump function");
        afterFCVBump(primaryConnection);

        // Downgrade FCV without restarting.
        jsTest.log.info("Downgrading the FCV to " + tojsononeline(lastLTSFCV));
        assert.commandWorked(getAdminDB().runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

        jsTest.log.info("Calling the afterPrimariesHaveRestarted function");
        afterPrimariesHaveRestarted(primaryConnection);
    }

    jsTest.log.info("Stopping the replica set");
    rst.stopSet();
}

/**
 * Simulates the upgrade procedure on a replica set from the "last-lts" version to the "latest"
 * version.
 */
export function testPerformUpgradeReplSet({
    startingNodeOptions = {},
    upgradeNodeOptions = {},
    setupFn,
    whenFullyDowngraded,
    whenSecondariesAreLatestBinary,
    whenBinariesAreLatestAndFCVIsLastLTS,
    whenFullyUpgraded,
}) {
    testPerformReplSetRollingRestart({
        startingVersion: lastLTSVersion,
        restartVersion: latestVersion,
        startingNodeOptions,
        restartNodeOptions: upgradeNodeOptions,
        setupFn,
        beforeRestart: whenFullyDowngraded,
        afterSecondariesHaveRestarted: whenSecondariesAreLatestBinary,
        afterPrimariesHaveRestarted: whenBinariesAreLatestAndFCVIsLastLTS,
        afterFCVBump: whenFullyUpgraded,
    });
}
