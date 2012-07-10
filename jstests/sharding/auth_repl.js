var replTest = new ReplSetTest({ nodes: 3, useHostName : false });
replTest.startSet({ oplogSize: 10, keyFile: 'jstests/libs/key1' });
replTest.initiate();
replTest.awaitSecondaryNodes();

var nodeCount = replTest.nodes.length;
var primary = replTest.getPrimary();

// Setup the database using replSet connection before setting the authentication
var conn = new Mongo(replTest.getURL());
var testDB = conn.getDB('test');
var testColl = testDB.user;

testColl.insert({ x: 1 });
testDB.runCommand({ getLastError: 1, w: nodeCount });

// Setup the cached connection for primary and secondary in DBClientReplicaSet
// before setting up authentication
var doc = testColl.findOne();
assert(doc != null);

conn.setSlaveOk();

doc = testColl.findOne();
assert(doc != null);

// Add admin user using direct connection to primary to simulate connection from remote host
var adminDB = primary.getDB('admin');
adminDB.addUser('user', 'user', false, nodeCount);
adminDB.auth('user', 'user');

var priTestDB = primary.getDB('test');
priTestDB.addUser('a', 'a', false, nodeCount);

// Authenticate the replSet connection
assert.eq(1, testDB.auth('a', 'a'));

jsTest.log('Sending an authorized query that should be ok');
conn.setSlaveOk(true);
doc = testColl.findOne();
assert(doc != null);

doc = testColl.find().readPref('secondary').next();
assert(doc != null);

conn.setSlaveOk(false);
doc = testColl.findOne();
assert(doc != null);

replTest.stopSet();

