// Tests that the usersInfo command reports the correct users when authSchemaVersion = 2

// bring up 2.4 mongod
var mongod = MongoRunner.runMongod({
    binVersion: '2.4',
    auth: ''
});

mongod.getDB('test').addUser({user: 'testUser', pwd: '12345', roles: ['readWrite']});
mongod.getDB('admin').addUser({user: 'adminUser',
                               pwd: '12345',
                               roles: ['userAdminAnyDatabase', 'clusterAdmin']});

// restart as 2.6
MongoRunner.stopMongod(mongod);
mongod = MongoRunner.runMongod({
    restart: mongod,
    binVersion: '2.6'
});

var admin = mongod.getDB('admin');
assert.eq(1, admin.auth('adminUser', '12345'));

// run the upgrade step once, to put us at auth schema version 2
var res = admin.runCommand({authSchemaUpgrade: 1, maxSteps: 1});
printjson(res);
assert.commandFailedWithCode(res, 77) // OperationIncomplete
assert(!res.done);

// confirm where we are in the upgrade process
res = admin.runCommand({getParameter: 1, authSchemaVersion: 1});
printjson(res);
assert.eq(1, res.ok);
assert.eq(2, res.authSchemaVersion);

// now, run usersInfo on the admin database
var usersInfo = admin.runCommand({usersInfo:1});
printjson(usersInfo);

// there should be a user reported in the output of usersInfo
assert.eq(1, usersInfo.users.length, 'usersInfo should find 1 user in the admin db');
assert.eq("adminUser", usersInfo.users[0].user, 'usersInfo did not find the adminUser@admin user');

MongoRunner.stopMongod(mongod);