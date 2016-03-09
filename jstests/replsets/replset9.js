

var rt = new ReplSetTest({name: "replset9tests", nodes: 1, oplogSize: 300});

var nodes = rt.startSet();
rt.initiate();
var master = rt.getPrimary();
var bigstring = Array(5000).toString();
var md = master.getDB('d');
var mdc = md['c'];

// idea: while cloner is running, update some docs and then immediately remove them.
// oplog will have ops referencing docs that no longer exist.

var doccount = 20000;
// Avoid empty extent issues
mdc.insert({_id: -1, x: "dummy"});

// Make this db big so that cloner takes a while.
print("inserting bigstrings");
var bulk = mdc.initializeUnorderedBulkOp();
for (i = 0; i < doccount; ++i) {
    mdc.insert({_id: i, x: bigstring});
}
assert.writeOK(bulk.execute());

// Insert some docs to update and remove
print("inserting x");
bulk = mdc.initializeUnorderedBulkOp();
for (i = doccount; i < doccount * 2; ++i) {
    bulk.insert({_id: i, bs: bigstring, x: i});
}
assert.writeOK(bulk.execute());

// add a secondary; start cloning
var slave = rt.add();
(function reinitiate() {
    var master = rt.nodes[0];
    var c = master.getDB("local")['system.replset'].findOne();
    var config = rt.getReplSetConfig();
    config.version = c.version + 1;
    var admin = master.getDB("admin");
    var cmd = {};
    var cmdKey = 'replSetReconfig';
    var timeout = timeout || 30000;
    cmd[cmdKey] = config;
    printjson(cmd);

    assert.soon(function() {
        var result = admin.runCommand(cmd);
        printjson(result);
        return result['ok'] == 1;
    }, "reinitiate replica set", timeout);
})();

print("initiation complete!");
var sc = slave.getDB('d')['c'];
slave.setSlaveOk();
master = rt.getPrimary();

print("updating and deleting documents");
bulk = master.getDB('d')['c'].initializeUnorderedBulkOp();
for (i = doccount * 4; i > doccount; --i) {
    bulk.find({_id: i}).update({$inc: {x: 1}});
    bulk.find({_id: i}).remove();
    bulk.insert({bs: bigstring});
}
assert.writeOK(bulk.execute());

print("finished");
// Wait for replication to catch up.
rt.awaitReplication(640000);
