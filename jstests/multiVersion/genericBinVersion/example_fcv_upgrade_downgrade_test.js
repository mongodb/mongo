/**
 * Example test showing how to test FCV upgrade/downgrade behavior for an FCV-gated feature flag.
 */

import "jstests/multiVersion/libs/multi_rs.js";

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

// Start up a replica set with the last-lts binary version.
const nodeOption = {
    binVersion: 'last-lts'
};
// Need at least 2 nodes because upgradeSet method needs to be able call step down
// with another primary-eligible node available.
const replSet = new ReplSetTest({nodes: [nodeOption, nodeOption]});
replSet.startSet();
replSet.initiate(null, null, {initiateWithDefaultElectionTimeout: true});

// Upgrade the set and enable the feature flag. The feature flag will be enabled as of
// the latest FCV. However, the repl set will still have FCV last-lts.
replSet.upgradeSet({binVersion: 'latest', setParameter: {featureFlagToaster: true}});

const primary = replSet.getPrimary();
const admin = primary.getDB('admin');

// The feature flag should not be enabled yet.
assert(!FeatureFlagUtil.isEnabled(admin, "Toaster"));

// Any pre-FCV-upgrade testing should occur here.

// Upgrade FCV and confirm the feature flag is now enabled.
assert.commandWorked(
    primary.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
assert(FeatureFlagUtil.isPresentAndEnabled(admin, "Toaster"));

// Any post-FCV-upgrade testing should occur here.

// Downgrade FCV and confirm the feature flag is now disabled.
assert.commandWorked(
    primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assert(!FeatureFlagUtil.isEnabled(admin, "Toaster"));

// Any post-FCV-downgrade testing should occur here.

replSet.stopSet();
