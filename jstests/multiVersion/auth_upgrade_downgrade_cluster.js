// Test that we can upgrade a cluster using keyfile auth from 2.6 to 2.8,
// downgrade back again and MONGODB-CR auth will continue to work
// regardless of server version. SCRAM-SHA-1 should work when mongos
// and the config servers are version 2.8.
load('./jstests/multiVersion/libs/auth_helpers.js');
load('./jstests/multiVersion/libs/multi_cluster.js');

var oldVersion = "2.6";
var newVersion = "2.8";

// enableBalancer: true is required to get auth working with 2.6
var options = {
    mongosOptions: {binVersion: oldVersion},
    configOptions: {binVersion: oldVersion},
    shardOptions: {binVersion: oldVersion},
    nopreallocj: 1,
    separateConfig: true,
    sync: true,
    useHostname: true,
    enableBalancer: true
}

var st = new ShardingTest({shards: 2, mongos: 1, other: options,
                           keyFile: 'jstests/libs/key1'});

var mongos = st.s;
var configConnStr = st._configDB;
var adminDB = mongos.getDB('admin');

adminDB.createUser({user: 'admin', pwd: 'password',
                    roles: jsTest.adminUserRoles});

assert(adminDB.auth({mechanism: 'MONGODB-CR',
                     user: 'admin', pwd: 'password'}));

// The balancer has to be off to upgrade the config server from v5 to v6.
st.stopBalancer();
// Upgrade config server from v5 to v6
var mongos = MongoRunner.runMongos({binVersion: newVersion,
                                    configdb: configConnStr,
                                    keyFile: 'jstests/libs/key1',
                                    upgrade: ""});

// Upgrade cluster
st.upgradeCluster(newVersion, {upgradeShards: true,
                               upgradeConfigs: true, upgradeMongos: true });

// Solve a race condition. During upgradeCluster mongos instances are
// restarted first, then the config servers. Restart the mongoses here
// again to force them to reconnect to the config servers quickly before
// we move on.
st.restartMongoses();

mongos = st.s;
adminDB = mongos.getDB('admin');

// We haven't run authSchemaUpgrade so MONGODB-CR should still work.
assert(adminDB.auth({mechanism: 'MONGODB-CR',
                     user: 'admin', pwd: 'password'}));

adminDB.createUser({user: 'user1', pwd: 'password',
                    roles: jsTest.adminUserRoles});
verifyUserDoc(adminDB, 'user1', true, false);

verifyAuth(adminDB, 'user1', 'password', true, true);

// Downgrade shards
st.upgradeCluster(oldVersion, {upgradeShards: true,
                               upgradeConfigs: false,
                               upgradeMongos: false});
// Downgrade configs
st.upgradeCluster(oldVersion, {upgradeShards: false,
                               upgradeConfigs: true,
                               upgradeMongos: false});
// Downgrade mongoses
st.upgradeCluster(oldVersion, {upgradeShards: false,
                               upgradeConfigs: false,
                               upgradeMongos: true});

mongos = st.s;
adminDB = mongos.getDB('admin');

// We never ran authSchemaUpgrade so MONGODB-CR should still work
// after downgrade. SCRAM, obviously, will no longer work.
assert(adminDB.auth({mechanism: 'MONGODB-CR',
                     user: 'admin', pwd: 'password'}));

adminDB.updateUser('user1', {pwd: 'newpassword',
                             roles: jsTest.adminUserRoles});
adminDB.createUser({user: 'user2',
                    pwd: 'password', roles: jsTest.adminUserRoles});

verifyUserDoc(adminDB, 'user1', true, false);
verifyUserDoc(adminDB, 'user2', true, false);

verifyAuth(adminDB, 'user1', 'newpassword', true, false);
verifyAuth(adminDB, 'user2', 'password', true, false);

st.stop();
