var replTest = new ReplSetTest({name: 'testSet', nodes: 2});

var nodes = replTest.startSet();
replTest.initiate();

var master = replTest.getPrimary();
db = master.getDB("foo");
db.foo.save({a: 1000});
replTest.awaitReplication();
replTest.awaitSecondaryNodes();

assert.eq(1, db.foo.count(), "setup");

var slaves = replTest.liveNodes.slaves;
assert(slaves.length == 1, "Expected 1 slave but length was " + slaves.length);
slave = slaves[0];

var commonOptions = {};
if (jsTest.options().keyFile) {
    commonOptions.username = jsTest.options().authUser;
    commonOptions.password = jsTest.options().authPassword;
}

var exitCode =
    MongoRunner.runMongoTool("mongodump",
                             Object.extend({
                                 host: slave.host,
                                 out: MongoRunner.dataDir + "/jstests_tool_dumpsecondary_external/",
                             },
                                           commonOptions));
assert.eq(0, exitCode, "mongodump failed to dump data from the secondary");

db.foo.drop();
assert.eq(0, db.foo.count(), "after drop");

exitCode =
    MongoRunner.runMongoTool("mongorestore",
                             Object.extend({
                                 host: master.host,
                                 dir: MongoRunner.dataDir + "/jstests_tool_dumpsecondary_external/",
                             },
                                           commonOptions));
assert.eq(0, exitCode, "mongorestore failed to restore data to the primary");

assert.soon("db.foo.findOne()", "no data after sleep");
assert.eq(1, db.foo.count(), "after restore");
assert.eq(1000, db.foo.findOne().a, "after restore 2");

resetDbpath(MongoRunner.dataDir + '/jstests_tool_dumpsecondary_external');

replTest.stopSet(15);
