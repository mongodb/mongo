/**
 * Tests a scenario such that admin.system.* collections that sort lexicographically before
 * admin.system.version are properly upgraded to the correct unique index version when upgraded to
 * 4.4. Collections cloned before the admin.system.version collection will build unique indexes in a
 * downgraded index format version. This test ensures that despite this, a 4.4 binary can
 * self-correct the problem when starting up.
 *
 * See SERVER-48054.
 */
(function() {
load('./jstests/multiVersion/libs/multi_rs.js');

const nodes = {
    n1: {binVersion: 'last-stable'},
    n2: {binVersion: 'last-stable'},
};

jsTest.log("Starting replica set in version 4.2");
const rst = new ReplSetTest({nodes: nodes});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const adminDB = primary.getDB('admin');

// Create a custom role and a user to populate the admin.system.roles, and admin.system.users
// collections.
assert.commandWorked(adminDB.runCommand({createRole: "readRole", privileges: [], roles: ['read']}));
assert.commandWorked(
    adminDB.runCommand({createUser: "readUser", pwd: "password", roles: ['readRole']}));

jsTest.log("Initial-syncing new node...");
const newNode = rst.add({
    rsConfig: {priority: 0, votes: 0},
    binVersion: 'last-stable',
});
rst.reInitiate();
rst.waitForState(newNode, ReplSetTest.State.SECONDARY);

jsTest.log("Upgrading replica set to 4.4...");

rst.upgradeSet({binVersion: "latest"});

jsTest.log("Checking unique index format version");

function checkFormatVersion(coll, indexName) {
    const formatVersion = coll.aggregate({$collStats: {storageStats: {}}})
                              .next()
                              .storageStats.indexDetails[indexName]
                              .metadata.formatVersion;
    assert.eq(
        formatVersion,
        12,
        `Expected ${coll.getFullName()} format version to be 12 for unique index '${indexName}'`);
}

const newNodeAdminDB = newNode.getDB('admin');
reconnect(newNodeAdminDB);
newNodeAdminDB.setSlaveOk(true);

checkFormatVersion(newNodeAdminDB.getCollection('system.roles'), 'role_1_db_1');
checkFormatVersion(newNodeAdminDB.getCollection('system.users'), 'user_1_db_1');

rst.stopSet();
})();
