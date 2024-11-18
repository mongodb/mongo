// Test that binary upgrade/downgrade works with auth schema version.
// Test that initial sync with users works with latest primary last-lts secondaries and vice
// versa.

import "jstests/multiVersion/libs/multi_rs.js";

import {ReplSetTest} from "jstests/libs/replsettest.js";

const keyfile = "jstests/libs/key1";
const authSchemaColl = "system.version";
const schemaVersion28SCRAM = 5;

// We need to upgrade the TestData's privileges so that the test itself can perform the
// necessary commands within rst.upgradeSet.
TestData.auth = true;
TestData.keyFile = keyfile;
TestData.authUser = "__system";
TestData.keyFileData = "foopdedoop";
TestData.authenticationDatabase = "local";

function testChangeBinariesWithAuthzSchemaDoc(originalBinVersion, updatedBinVersion) {
    const rst = new ReplSetTest(
        {nodes: 2, nodeOptions: {binVersion: originalBinVersion, auth: ''}, keyFile: keyfile});
    rst.startSet();
    rst.initiate();

    // Create a user, which is a necessary precondition for requiring authorization schema document
    // on lower binVersion binaries .
    let primary = rst.getPrimary();
    let adminDB = primary.getDB("admin");
    assert.commandWorked(adminDB.runCommand(
        {createUser: "admin", pwd: "admin", roles: [{role: "root", db: "admin"}]}));

    // If updatedBinVersion is last-lts, then we need to downgrade FCV before downgrading the
    // binary.
    if (updatedBinVersion === 'last-lts') {
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    }

    // Query the authorization schema doc to ensure that it exists and is set to
    // schemaVersion28SCRAM. On 'last-lts' binaries that are being upgraded, this doc
    // should have been created during `createUser`. On `latest` binaries that are being
    // downgraded, it should have been created during FCV downgrade since at least 1 user
    // document (`admin`) existed.
    const currentVersion = adminDB[authSchemaColl].findOne({_id: 'authSchema'}).currentVersion;
    assert.eq(currentVersion, schemaVersion28SCRAM);

    // Change binaries to updatedBinVersion - this may constitute an upgrade or a downgrade.
    // Last-lts binaries write and read the authorization schema document, while latest binaries
    // should only write it down during FCV downgrade if there are any user or role docs on-disk.
    // Latest binaries never read from it.
    // Therefore, upgrade should always work without issues while downgrade
    // should work as long as FCV is properly downgraded before binary downgrade.
    rst.upgradeSet({binVersion: updatedBinVersion, keyFile: keyfile});

    // Retrieve the new primary and run usersInfo to make sure everything works normally.
    primary = rst.getPrimary();
    adminDB = primary.getDB("admin");
    const usersInfoReply = assert.commandWorked(adminDB.runCommand({usersInfo: 1}));
    assert(usersInfoReply.hasOwnProperty("users"));
    assert.eq(usersInfoReply.users.length, 1);

    rst.stopSet();
}

function testMixedVersionInitialSync(primaryBinVersion, newNodeBinVersion) {
    // Create a single-node replica set with binVersion primaryBinVersion.
    const rst = new ReplSetTest(
        {nodes: 1, nodeOptions: {binVersion: primaryBinVersion, auth: ''}, keyFile: keyfile});
    rst.startSet();
    rst.initiate();

    // Create a user, which should automatically create an authorization schema document.
    let primary = rst.getPrimary();
    let adminDB = primary.getDB("admin");
    assert.commandWorked(adminDB.runCommand(
        {createUser: "admin", pwd: "admin", roles: [{role: "root", db: "admin"}]}));

    // If newNodeBinVersion is 'last-lts', then we need to downgrade FCV prior to adding the new
    // node.
    if (newNodeBinVersion === 'last-lts') {
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}));
    }

    // Query the authorization schema doc to ensure that it exists and is set to
    // schemaVersion28SCRAM. On 'last-lts' binaries that are being upgraded, this doc
    // should have been created during `createUser`. On `latest` binaries that are being
    // downgraded, it should have been created during FCV downgrade.
    const currentVersion = adminDB[authSchemaColl].findOne({_id: 'authSchema'}).currentVersion;
    assert.eq(currentVersion, schemaVersion28SCRAM);

    // Add a newNodeBinVersion node to the replica set and check that initial sync succeeds. The new
    // node will have no priority or
    let secondary = rst.add(
        {binVersion: newNodeBinVersion, keyFile: keyfile, rsConfig: {votes: 0, priority: 0}});

    // Reinitiate the replica set and check that initial sync has completed on the secondary.
    rst.reInitiate();
    assert.commandWorked(
        primary.getDB("test").coll.insert({awaitRepl: true}, {writeConcern: {w: 2}}));
    rst.awaitReplication();
    rst.awaitSecondaryNodes();

    // Run usersInfo on the secondary node and check that 1 user (admin) has been replicated.
    secondary = rst.getSecondary();
    const secondaryAdminDB = secondary.getDB("admin");
    const usersInfoReply = assert.commandWorked(secondaryAdminDB.runCommand({usersInfo: 1}));
    assert(usersInfoReply.hasOwnProperty("users"));
    assert.eq(usersInfoReply.users.length, 1);

    rst.stopSet();
}

// Upgrade
testChangeBinariesWithAuthzSchemaDoc('last-lts', 'latest');

// Downgrade
testChangeBinariesWithAuthzSchemaDoc('latest', 'last-lts');

// Initial sync for a last-lts node from an latest primary.
testMixedVersionInitialSync('latest', 'last-lts');

// Initial sync for a latest node from a last-lts primary.
testMixedVersionInitialSync('last-lts', 'latest');
