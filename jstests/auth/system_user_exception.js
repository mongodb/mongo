// Test the special handling of the __system user
// works when pw auth mechanisms are disabled.

var port = allocatePorts(1)[0];

// Start mongod with no authentication mechanisms enabled
var m = MongoRunner.runMongod({keyFile: "jstests/libs/key1",
                               port: port,
                               dbpath: MongoRunner.dataDir + "/no-authmechs",
                               setParameter: "authenticationMechanisms="});

// Verify that it's possible to authenticate the __system@local user
assert.eq(1, m.getDB("local").auth("__system", "foopdedoop"), 1);
assert.eq(0, m.getDB("local").auth("__system", "foopdedoo"), 0);
assert.eq(1, m.getDB("local").auth({user: "__system", pwd: "foopdedoop", mechanism: "MONGODB-CR"}));

// Verify that it is not possible to authenticate other users
m.getDB("test").createUser({user: "guest" , pwd: "guest", roles: jsTest.readOnlyUserRoles});
assert.eq(0, m.getDB("test").auth("guest", "guest"));

MongoRunner.stopMongod(port);
