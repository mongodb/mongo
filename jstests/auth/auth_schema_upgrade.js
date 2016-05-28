// Standalone test of authSchemaUpgrade
load('./jstests/multiVersion/libs/auth_helpers.js');

var setupCRUsers = function(conn) {
    jsTest.log("setting up legacy users");
    var adminDB = conn.getDB('admin');

    adminDB.system.version.update({_id: "authSchema"}, {"currentVersion": 3}, {upsert: true});

    adminDB.createUser({user: 'user1', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(adminDB.auth({mechanism: 'MONGODB-CR', user: 'user1', pwd: 'pass'}));

    adminDB.createUser({user: 'user2', pwd: 'pass', roles: jsTest.adminUserRoles});
    assert(adminDB.auth({mechanism: 'MONGODB-CR', user: 'user2', pwd: 'pass'}));

    // Add $external no-op user to verify that it does not affect
    // authSchemaUpgrade SERVER-18475
    adminDB.getSiblingDB('$external').createUser({user: "evil", roles: []});

    jsTest.log("Verifying user documents before upgrading");

    // We haven't run authSchemaUpgrade so there shouldn't be
    // any stored SCRAM-SHA-1 credentials.
    verifyUserDoc(adminDB, 'user1', true, false);
    verifyUserDoc(adminDB, 'user2', true, false);
    verifyUserDoc(adminDB.getSiblingDB('$external'), "evil", false, false, true);

    adminDB.updateUser('user1', {pwd: 'newpass', roles: jsTest.adminUserRoles});
    verifyAuth(adminDB, 'user1', 'newpass', true, true);

    verifyUserDoc(adminDB, 'user1', true, false);
};

var verifySchemaUpgrade = function(adminDB) {
    // All users should only have SCRAM credentials.
    verifyUserDoc(adminDB, 'user1', false, true);
    verifyUserDoc(adminDB, 'user2', false, true);
    verifyUserDoc(adminDB.getSiblingDB('$external'), "evil", false, false, true);

    // After authSchemaUpgrade MONGODB-CR no longer works.
    verifyAuth(adminDB, 'user1', 'newpass', false, true);
    verifyAuth(adminDB, 'user2', 'pass', false, true);
};

var runAndVerifySchemaUpgrade = function(conn) {
    jsTest.log("run authSchemaUpgrade");
    var adminDB = conn.getDB('admin');

    assert.commandWorked(adminDB.runCommand('authSchemaUpgrade'));
    verifySchemaUpgrade(adminDB);
};

var testAuthSchemaUpgrade = function(conn) {
    setupCRUsers(conn);
    runAndVerifySchemaUpgrade(conn);
};

// Test authSchemaUpgrade and upgrade shards
var testUpgradeShards = function(mongos, shard) {
    setupCRUsers(shard);

    assert.commandWorked(mongos.adminCommand({"authSchemaUpgrade": 1, "upgradeShards": 1}));
    verifySchemaUpgrade(shard.getDB('admin'));
};

jsTest.log('Test authSchemUpgrade standalone');
var conn = MongoRunner.runMongod();
testAuthSchemaUpgrade(conn);
MongoRunner.stopMongod(conn);

jsTest.log('Test authSchemUpgrade sharded');
var dopts = {smallfiles: "", nopreallocj: ""};
var st = new ShardingTest({
    shards: 1,
    mongos: 1,
    config: 1,
    useHostname: false,  // Needed when relying on the localhost exception
    other: {shardOptions: dopts, configOptions: dopts, mongosOptions: {verbose: 1}}
});
testAuthSchemaUpgrade(st.s);
testUpgradeShards(st.s, st.shard0);
st.stop();
