
import "jstests/multiVersion/libs/multi_rs.js";

export function testPerformUpgradeDowngradeReplSet({
    setupFn,
    whenFullyDowngraded,
    whenSecondariesAreLatestBinary,
    whenBinariesAreLatestAndFCVIsLastLTS,
    whenFullyUpgraded
}) {
    const lastLTSVersion = {binVersion: "last-lts"};
    const latestVersion = {binVersion: "latest"};

    const rst = new ReplSetTest({
        name: jsTestName(),
        nodes: [
            {...lastLTSVersion},
            {...lastLTSVersion},
        ],
    });
    rst.startSet();
    rst.initiate();

    let primaryConnection = rst.getPrimary();
    const getAdminDB = () => primaryConnection.getDB("admin");

    setupFn(primaryConnection);

    whenFullyDowngraded(primaryConnection);

    // Upgrade the secondaries only.
    rst.upgradeSecondaries({...latestVersion});
    primaryConnection = rst.getPrimary();

    whenSecondariesAreLatestBinary(primaryConnection = rst.getPrimary());

    // Upgrade the primaries.
    rst.upgradeSet({...latestVersion});
    primaryConnection = rst.getPrimary();

    whenBinariesAreLatestAndFCVIsLastLTS(primaryConnection);

    // Upgrade the FCV.
    assert.commandWorked(
        getAdminDB().runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    whenFullyUpgraded(primaryConnection);

    // Downgrade FCV without restarting.
    assert.commandWorked(
        getAdminDB().runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));

    whenBinariesAreLatestAndFCVIsLastLTS(primaryConnection);

    rst.stopSet();
}
