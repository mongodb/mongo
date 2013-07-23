// Test that replSetStepDown filters out non-electable nodes
var replTest = new ReplSetTest({ name: 'testSet', nodes: 2 });
var nodes = replTest.startSet();


// setup config
var c = replTest.getReplSetConfig();
c.members[1].priority = 0; // not electable
replTest.initiate(c);

var master = replTest.getMaster();
var testDB = master.getDB('test');
var firstPrimary = testDB.isMaster().primary

// do a write to allow stepping down of the primary;
// otherwise, the primary will refuse to step down
testDB.foo.insert({x:1});
replTest.awaitReplication();

// stepdown should fail since there is no-one to elect within 10 secs
testDB.adminCommand({replSetStepDown:5});
assert(new Mongo(firstPrimary).getDB("a").isMaster().ismaster, "not master")

// step down the primary asyncronously so it doesn't kill this test
var command = "tojson(db.adminCommand({replSetStepDown:1000, force:true}));"
// set db so startParallelShell works
db = testDB;
var waitfunc = startParallelShell(command);
sleep(100) // startParallelShell doesn't block

// check that the old primary is no longer master
assert.soon( function() {
    var isMaster = new Mongo(firstPrimary).getDB("a").isMaster();
    printjson(isMaster);
    return !(isMaster.ismaster);
  }, "they shouldn't be master, but are")
// stop
replTest.stopSet();
