// test that rotating logs creates a new log file for the old logs
// then redo that test with auth confirming that the needed roles are as expected

var logDir  = "/tmp";
var logPath = logDir + "/testlog";

// start up a mongod with a log path
var mongodInstance = MongoRunner.runMongod({logpath: logPath});
var newDB = mongodInstance.getDB("log_rotate");
newDB.dropDatabase();
var i;
for (i = 0; i < 100; i++) { 
    newDB.logtest.insert({thing: i});
}

// rotate logs and see that file count in the log directory increases by one
var pre_count  = listFiles(logDir).length;
assert.commandWorked(newDB.adminCommand({logRotate: 1}));
var post_count = listFiles(logDir).length;
assert.eq(pre_count + 1, post_count);

// shut down mongod
MongoRunner.stopMongod(mongodInstance.port);

// now again but with auth to confirm necessary roles are as expected
var authzErrorCode = 13;
var dbAdminUsername = 'admin';
var dbAdminPassword = 'admin';
mongodInstance = MongoRunner.runMongod({logpath: logPath, auth: ""});

// create a user to create logs
newDB = mongodInstance.getDB('admin');
newDB.dropDatabase();
newDB.createUser({'user':dbAdminUsername, 'pwd':dbAdminPassword, 'roles':['userAdminAnyDatabase']});
newDB.auth(dbAdminUsername, dbAdminPassword);
pre_count  = listFiles(logDir).length;

// create user without the clusterAdmin role and make sure that logRotate fails
newDB.createUser({'user':'nonClusterAdmin', 'pwd':'nonClusterAdmin', 'roles':['dbAdmin','read','readWrite','dbAdmin','userAdmin']});
newDB.auth('nonClusterAdmin', 'nonClusterAdmin');
assert.commandFailedWithCode(newDB.runCommand('logRotate'), authzErrorCode);

// remove the above user that was created
newDB.auth(dbAdminUsername, dbAdminPassword);
newDB.dropUser('nonClusterAdmin');

// create user with the clusterAdmin role and ensure that logRotate passes
newDB.createUser({'user':'clusterAdmin', 'pwd':'clusterAdmin', 'roles':['clusterAdmin']});
newDB.auth('clusterAdmin', 'clusterAdmin');
assert.commandWorked(newDB.runCommand('logRotate'));
post_count = listFiles(logDir).length;
assert.eq(pre_count + 1, post_count);

// shut down mongod
MongoRunner.stopMongod(mongodInstance.port);
