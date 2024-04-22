/**
 * Tests the behaviour of defaultMaxTimeMS during FCV upgrade / downgrade in a replica set.
 */

import "jstests/multiVersion/libs/multi_rs.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

function createUserAndLogin(adminDB, user, password, roles, newConnection = false) {
    // Error 51003 is returned when the user already exists.
    assert.commandWorkedOrFailedWithCode(
        adminDB.runCommand({createUser: user, pwd: password, roles: roles}), 51003);
    const loginDB = newConnection ? new Mongo(adminDB.getMongo().host).getDB('admin') : adminDB;
    loginDB.logout();
    assert.eq(1, loginDB.auth(user, password));
    return loginDB.getMongo();
}

TestData.auth = true;
TestData.keyFile = "jstests/libs/key1";
TestData.authUser = "__system";
TestData.keyFileData = "foopdedoop";
TestData.authenticationDatabase = "local";
// Start up a replica set with the last-lts binary version.
const nodeOption = {
    binVersion: 'last-lts'
};
// Need at least 2 nodes because upgradeSet method needs to be able call step down
// with another primary-eligible node available.
const replSet = new ReplSetTest({nodes: [nodeOption, nodeOption]});
replSet.startSet();
replSet.initiate();

// Upgrade the set and enable the feature flag. The feature flag will be enabled as of
// the latest FCV. However, the repl set will still have FCV last-lts.
replSet.upgradeSet({binVersion: 'latest', setParameter: {featureFlagDefaultReadMaxTimeMS: true}});

// Upgrade may cause a change in primary, get connection to the new primary.
const primary = replSet.getPrimary();
const adminDB = primary.getDB('admin');
const nonBypassConn =
    createUserAndLogin(adminDB, 'nonBypassUser', 'password', ['readAnyDatabase'], true)
        .getDB('admin');

// The feature flag should not be enabled yet.
assert(!FeatureFlagUtil.isEnabled(adminDB, "DefaultReadMaxTimeMS"));

const commandToRun = {
    sleep: 1,
    millis: 2000
};

// Pre-FCV-upgrade testing.
// Setting the cluster parameter should fail.
assert.commandFailedWithCode(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 500}}}),
    ErrorCodes.BadValue);
// A command which takes longer than the failed set cluster parameter value should succeed.
assert.commandWorked(nonBypassConn.runCommand(commandToRun));

// Upgrade FCV and confirm the feature flag is now enabled.
assert.commandWorked(
    primary.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
assert(FeatureFlagUtil.isPresentAndEnabled(adminDB, "DefaultReadMaxTimeMS"));

// Post-FCV-upgrade testing.
assert.commandWorked(
    adminDB.runCommand({setClusterParameter: {defaultMaxTimeMS: {readOperations: 500}}}));
// A command which takes longer than defaultMaxTimeMS.readOperations should fail.
assert.commandFailedWithCode(nonBypassConn.runCommand(commandToRun), ErrorCodes.MaxTimeMSExpired);

// Downgrade FCV and confirm the feature flag is now disabled.
assert.commandWorked(
    primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
assert(!FeatureFlagUtil.isEnabled(adminDB, "DefaultReadMaxTimeMS"));

// Any post-FCV-downgrade testing should occur here.
// Even without explicitly unsetting defaultMaxTimeMS, queries should not be applying it after
// downgrade.
assert.commandWorked(nonBypassConn.runCommand(commandToRun));

replSet.stopSet();
