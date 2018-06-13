var replTest = new ReplSetTest({nodes: 3, useHostName: false, keyFile: 'jstests/libs/key1'});
replTest.startSet({oplogSize: 10});
replTest.initiate();
replTest.awaitSecondaryNodes();

var nodeCount = replTest.nodes.length;
var primary = replTest.getPrimary();

// Setup the database using replSet connection before setting the authentication
var conn = new Mongo(replTest.getURL());
var testDB = conn.getDB('test');
var adminDB = conn.getDB('admin');
var testColl = testDB.user;

// Setup the cached connection for primary and secondary in DBClientReplicaSet
// before setting up authentication
assert.commandWorked(adminDB.runCommand({replSetGetStatus: 1}));

conn.setSlaveOk();
assert.commandWorked(adminDB.runCommand({replSetGetStatus: 1}));

// Add admin user using direct connection to primary to simulate connection from remote host
var priAdminDB = primary.getDB('admin');
priAdminDB.createUser({user: 'user', pwd: 'user', roles: jsTest.adminUserRoles},
                      {w: nodeCount, wtimeout: 30000});
priAdminDB.auth('user', 'user');

var priTestDB = primary.getDB('test');
priTestDB.createUser({user: 'a', pwd: 'a', roles: jsTest.basicUserRoles},
                     {w: nodeCount, wtimeout: 30000});

// Authenticate the replSet connection
assert.eq(1, testDB.auth('a', 'a'));

jsTest.log('Sending an authorized query that should be ok');
assert.writeOK(testColl.insert({x: 1}, {writeConcern: {w: nodeCount}}));

conn.setSlaveOk(true);
doc = testColl.findOne();
assert(doc != null);

doc = testColl.find().readPref('secondary').next();
assert(doc != null);

conn.setSlaveOk(false);
doc = testColl.findOne();
assert(doc != null);

var queryToPriShouldFail = function() {
    conn.setSlaveOk(false);

    assert.throws(function() {
        testColl.findOne();
    });

    // should still not work even after retrying
    assert.throws(function() {
        testColl.findOne();
    });
};

var queryToSecShouldFail = function() {
    conn.setSlaveOk(true);

    assert.throws(function() {
        testColl.findOne();
    });

    // should still not work even after retrying
    assert.throws(function() {
        testColl.findOne();
    });

    // Query to secondary using readPref
    assert.throws(function() {
        testColl.find().readPref('secondary').next();
    });

    // should still not work even after retrying
    assert.throws(function() {
        testColl.find().readPref('secondary').next();
    });
};

assert(testDB.logout().ok);

jsTest.log('Sending an unauthorized query that should fail');
queryToPriShouldFail();
queryToSecShouldFail();

// Repeat logout test, with secondary first, then primary
assert.eq(1, testDB.auth('a', 'a'));
assert(testDB.logout().ok);

// re-initialize the underlying connections to primary and secondary
jsTest.log('Sending an unauthorized query that should still fail');
queryToSecShouldFail();
queryToPriShouldFail();

// Repeat logout test, now with the cached secondary down
assert.eq(1, testDB.auth('a', 'a'));

// Find out the current cached secondary in the repl connection
conn.setSlaveOk(true);
var serverInfo = testColl.find().readPref('secondary').explain().serverInfo;
var secNodeIdx = -1;
var secPortStr = serverInfo.port.toString();

for (var x = 0; x < nodeCount; x++) {
    var nodePortStr = replTest.nodes[x].host.split(':')[1];

    if (nodePortStr == secPortStr) {
        secNodeIdx = x;
    }
}

assert(secNodeIdx >= 0);  // test sanity check

// Kill the cached secondary
replTest.stop(secNodeIdx, 15, {auth: {user: 'user', pwd: 'user'}});

assert(testDB.logout().ok);

replTest.restart(secNodeIdx);
replTest.awaitSecondaryNodes();

jsTest.log('Sending an unauthorized query after restart that should still fail');
queryToSecShouldFail();
queryToPriShouldFail();

replTest.stopSet();
