// This tests a few different things:
// 1. That we can upgrade a cluster with keyfile auth from 2.6 to 2.8.
// 2. That after upgrade but before authSchemaUpgrade both MONGODB-CR and
//    SCRAM-SHA-1 work.
// 3. That in 2. user documents only have MONGODB-CR credentials.
// 4. That after authSchemaUpgrade only SCRAM-SHA-1 works.
// 5. That in 4. user documents only have SCRAM-SHA-1 credentials.
// 6. That after authSchemaUpgrade a downgraded cluster can no longer auth.
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

// Step 1. Create a new 2.6 cluster with keyfile auth, add a user, test
// that auth works, upgrade the cluster to 2.8 and test auth again.
var st = new ShardingTest({shards: 2, mongos: 1, other: options,
                           keyFile: 'jstests/libs/key1'});

var mongos = st.s;
var configConnStr = st._configDB;
var adminDB = mongos.getDB('admin');

adminDB.createUser({user: 'user1', pwd: 'password',
                    roles: jsTest.adminUserRoles});

assert(adminDB.auth({mechanism: 'MONGODB-CR',
                     user: 'user1', pwd: 'password'}));
// 2.6 doesn't support SCRAM so this should fail.
assert.eq(false, adminDB.auth({mechanism: 'SCRAM-SHA-1',
                               user: 'user1', pwd: 'password'}));

// Authenticate so stopBalancer will work. Don't specify mechanism here to test
// that a 2.8 shell uses the correct default for a 2.6 server.
assert(adminDB.auth({user: 'user1', pwd: 'password'}));
// The balancer has to be off to upgrade the config server from v5 to v6.
st.stopBalancer();

// Upgrade config server from v5 to v6.
var mongos = MongoRunner.runMongos({binVersion: newVersion,
                                    configdb: configConnStr,
                                    keyFile: 'jstests/libs/key1',
                                    upgrade: ""});

// Upgrade both config server and mongos. Both are necessary for SCRAM to work.
st.upgradeCluster(newVersion, {upgradeShards: false,
                               upgradeConfigs: true,
                               upgradeMongos: true});

// Solve a race condition. During upgradeCluster mongos instances are
// restarted first, then the config servers. Restart the mongoses here
// again to force them to reconnect to the config servers quickly before
// we move on.
st.restartMongoses();

mongos = st.s;
adminDB = mongos.getDB('admin');

// Step 2. We haven't run authSchemaUpgrade so MONGODB-CR should still work.
assert(adminDB.auth({mechanism: 'MONGODB-CR', user: 'user1',
                     pwd: 'password'}));
verifyUserDoc(adminDB, 'user1', true, false);

adminDB.updateUser('user1', {pwd: 'newpassword',
                             roles: jsTest.adminUserRoles});
adminDB.createUser({user: 'user2', pwd: 'password',
                    roles: jsTest.adminUserRoles});

// Step 3. We haven't run authSchemaUpgrade so there shouldn't be
// any stored SCRAM-SHA-1 credentials.
verifyUserDoc(adminDB, 'user1', true, false);
verifyUserDoc(adminDB, 'user2', true, false);

// Regardless, SCRAM-SHA-1 and MONGODB-CR should still work.
verifyAuth(adminDB, 'user1', 'newpassword', true, true);
verifyAuth(adminDB, 'user2', 'password', true, true);

// Upgrade the shards, then run the same tests again.
st.upgradeCluster(newVersion, {upgradeShards: true,
                               upgradeConfigs: false,
                               upgradeMongos: false});

assert(adminDB.auth({mechanism: 'MONGODB-CR',
                     user: 'user1', pwd: 'newpassword'}));
verifyUserDoc(adminDB, 'user1', true, false);
verifyUserDoc(adminDB, 'user2', true, false);

adminDB.updateUser('user2', {pwd: 'newpassword',
                             roles: jsTest.adminUserRoles});
adminDB.createUser({user: 'user3', pwd: 'password',
                    roles: jsTest.adminUserRoles });

verifyUserDoc(adminDB, 'user2', true, false);
verifyUserDoc(adminDB, 'user3', true, false);

verifyAuth(adminDB, 'user2', 'newpassword', true, true);
verifyAuth(adminDB, 'user3', 'password', true, true);

// Step 4. After authSchemaUpgrade MONGODB-CR will no longer work since
// MONGODB-CR credentials have been removed.
assert(adminDB.runCommand('authSchemaUpgrade'));

adminDB.createUser({user: 'user4', pwd: 'password',
                    roles: jsTest.adminUserRoles});

verifyAuth(adminDB, 'user4', 'password', false, true);
verifyAuth(adminDB, 'user1', 'newpassword', false, true);

// Step 5. After authSchemaUpgrade there shouldn't be MONGODB-CR credentials.
verifyUserDoc(adminDB, 'user1', false, true);
verifyUserDoc(adminDB, 'user2', false, true);
verifyUserDoc(adminDB, 'user3', false, true);
verifyUserDoc(adminDB, 'user4', false, true);

assert.eq(false, adminDB.auth({mechanism: 'NOT-A-MECHANISM',
                               user: 'user4', pwd: 'password'}));

// Step 6. Finally, downgrade the cluster back to 2.6.

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
                               upgradeMongos: true }); 

mongos = st.s;
adminDB = mongos.getDB('admin');

// Since we ran authSchemaUpgrade before the downgrade it is no longer
// possible to authenticate.
verifyAuth(adminDB, 'user4', 'password', false, false);
verifyAuth(adminDB, 'user1', 'newpassword', false, false);

st.stop();
