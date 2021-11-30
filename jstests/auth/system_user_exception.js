// Test the special handling of the __system user
// works when the SCRAM-SHA-1 pw auth mechanisms are disabled.
(function() {
"use strict";

// Start mongod with no authentication mechanisms enabled
var m = MongoRunner.runMongod(
    {keyFile: "jstests/libs/key1", setParameter: "authenticationMechanisms=PLAIN"});

// Verify that it's possible to use SCRAM-SHA-1 to authenticate as the __system@local user
assert.eq(1,
          m.getDB("local").auth({user: "__system", pwd: "foopdedoop", mechanism: "SCRAM-SHA-1"}));

// Verify that it is not possible to authenticate other users
m.getDB("test").runCommand({createUser: "guest", pwd: "guest", roles: jsTest.readOnlyUserRoles});
assert.eq(0, m.getDB("test").auth({user: "guest", pwd: "guest", mechanism: "SCRAM-SHA-1"}));

MongoRunner.stopMongod(m);
})();
