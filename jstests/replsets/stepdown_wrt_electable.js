// Test that replSetStepDown filters out non-electable nodes
var replTest = new ReplSetTest({name: 'testSet', nodes: 2});
var nodes = replTest.startSet();

// setup config
var c = replTest.getReplSetConfig();
c.members[1].priority = 0;  // not electable
replTest.initiate(c);

var master = replTest.getPrimary();
var testDB = master.getDB('test');
var firstPrimary = testDB.isMaster().primary;

// do a write to allow stepping down of the primary;
// otherwise, the primary will refuse to step down
testDB.foo.insert({x: 1});
replTest.awaitReplication();

// stepdown should fail since there is no-one to elect within 10 secs
testDB.adminCommand({replSetStepDown: 5});
assert(master.getDB("a").isMaster().ismaster, "not master");

// step down the primary asyncronously so it doesn't kill this test
var wait = startParallelShell("db.adminCommand({replSetStepDown:1000, force:true})", master.port);
var exitCode = wait({checkExitSuccess: false});
assert.neq(0, exitCode, "expected replSetStepDown to close the shell's connection");

// check that the old primary is no longer master
assert.soon(function() {
    try {
        var isMaster = master.getDB("a").isMaster();
        printjson(isMaster);
        return !(isMaster.ismaster);
    } catch (e) {
        return false;
    }
}, "they shouldn't be master, but are");

// stop
replTest.stopSet();
