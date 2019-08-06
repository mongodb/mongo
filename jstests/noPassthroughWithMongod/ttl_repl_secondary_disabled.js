/**
 * Test TTL docs are not deleted from secondaries directly
 * @tags: [requires_replication]
 */

// Skip db hash check because secondary will have extra document due to the usage of the godinsert
// command.
TestData.skipCheckDBHashes = true;

var rt = new ReplSetTest({name: "ttl_repl", nodes: 2});

// setup set
var nodes = rt.startSet();
rt.initiate();
var master = rt.getPrimary();
rt.awaitSecondaryNodes();
var slave1 = rt.getSecondary();

// shortcuts
var masterdb = master.getDB('d');
var slave1db = slave1.getDB('d');
var mastercol = masterdb['c'];
var slave1col = slave1db['c'];

// create TTL index, wait for TTL monitor to kick in, then check things
mastercol.ensureIndex({x: 1}, {expireAfterSeconds: 10});

rt.awaitReplication();

// increase logging
assert.commandWorked(slave1col.getDB().adminCommand({setParameter: 1, logLevel: 1}));

// insert old doc (10 minutes old) directly on secondary using godinsert
assert.commandWorked(slave1col.runCommand(
    "godinsert", {obj: {_id: new Date(), x: new Date((new Date()).getTime() - 600000)}}));
assert.eq(1, slave1col.count(), "missing inserted doc");

sleep(70 * 1000);  // wait for 70seconds
assert.eq(1, slave1col.count(), "ttl deleted my doc!");

// finish up
rt.stopSet();
