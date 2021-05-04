// Test the implicit default write concern.
load("jstests/replsets/rslib.js");

(function() {
"use strict";
// Test write concern defaults
var replTest = new ReplSetTest({name: 'testSet', nodes: 3});

var nodes = replTest.startSet();
replTest.initiate();

var config = replTest.getReplSetConfigFromNode();
config.version++;
config.settings = {};
config.settings.heartbeatTimeoutSecs = 15;
// Prevent node 2 from becoming primary, as we will attempt to set it to hidden later.
config.members[2].priority = 0;
reconfig(replTest, config);

//
var primary = replTest.getPrimary();
replTest.awaitSecondaryNodes();
var testDB = "foo";

// Initial replication
// TODO SERVER-56633: Remove manual write concern once we start using the new implicit default write
// concern.
primary.getDB("barDB").bar.save({a: 1},
                                {writeConcern: {w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}});
replTest.awaitReplication();

// These writes should be replicated immediately
var docNum = 5000;
var bulk = primary.getDB(testDB).foo.initializeUnorderedBulkOp();
for (var n = 0; n < docNum; n++) {
    bulk.insert({n: n});
}

// If you want to test failure, just add values for w and wtimeout (e.g. w=1)
// to the following command. This will override the default set above and
// prevent replication from happening in time for the count tests below.
// TODO SERVER-56633: Remove manual write concern once we start using the new implicit default write
// concern.
var result = bulk.execute({w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS});
var wcError = result.getWriteConcernError();

if (wcError != null) {
    print("\WARNING getLastError timed out and should not have: " + result.toString());
    print("This machine seems extremely slow. Stopping test without failing it\n");
    replTest.stopSet();
    return;
}

var secondaries = replTest.getSecondaries();
secondaries[0].setSecondaryOk();
secondaries[1].setSecondaryOk();

var secondary0Count = secondaries[0].getDB(testDB).foo.find().itcount();
assert(secondary0Count == docNum,
       "Secondary 0 has " + secondary0Count + " of " + docNum + " documents!");

var secondary1Count = secondaries[1].getDB(testDB).foo.find().itcount();
assert(secondary1Count == docNum,
       "Secondary 1 has " + secondary1Count + " of " + docNum + " documents!");

var primary1Count = primary.getDB(testDB).foo.find().itcount();
assert(primary1Count == docNum, "Master has " + primary1Count + " of " + docNum + " documents!");

print("replset5.js reconfigure with hidden=1");
config = primary.getDB("local").system.replset.findOne();

assert.eq(15, config.settings.heartbeatTimeoutSecs);

config.version++;
config.members[2].hidden = 1;

primary = reconfig(replTest, config);

config = primary.getSiblingDB("local").system.replset.findOne();
assert.eq(config.members[2].hidden, true);

replTest.stopSet();
}());
