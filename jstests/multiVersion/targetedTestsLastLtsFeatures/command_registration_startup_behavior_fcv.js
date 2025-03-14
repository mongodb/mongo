/**
 * Tests that commands gated on a feature flag does not depend on FCV at startup.
 */

import "jstests/multiVersion/libs/multi_rs.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: [{binVersion: "last-lts"}, {binVersion: "last-lts"}],
});
rst.startSet();
rst.initiate();

// This testCmd is gated by gFeatureFlagBlender which is enabled on latest version.
const testCmd = {
    testCommandFeatureFlaggedOnLatestFCV: 1
};

// The testCmd should not be registered when the node is on the last-lts binary.
assert.commandFailedWithCode(rst.getPrimary().adminCommand(testCmd), ErrorCodes.CommandNotFound);

jsTest.log("Upgrading the replica set to latest.");
rst.upgradeSet({binVersion: 'latest'});
jsTest.log("Upgrade complete.");

// Check that the FCV is still on lastLTS since we only upgraded the binary to latest.
checkFCV(rst.getPrimary().getDB("admin"), lastLTSFCV);

// The testCmd should now be registered since the feature flag is enabled, even though the FCV
// hasn't been upgraded.
assert.commandWorked(rst.getPrimary().adminCommand(testCmd));

rst.stopSet();
