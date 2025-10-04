// Tests that mongos can autodiscover a config server replica set when the only node it knows about
// is not the primary.

import {ReplSetTest} from "jstests/libs/replsettest.js";

let rst = new ReplSetTest({name: "configRS", nodes: 3, nodeOptions: {configsvr: "", storageEngine: "wiredTiger"}});
rst.startSet();
let conf = rst.getReplSetConfig();
conf.members[1].priority = 0;
conf.members[2].priority = 0;
conf.writeConcernMajorityJournalDefault = true;
rst.initiate(conf);

let seedList = rst.name + "/" + rst.nodes[1].host; // node 1 is guaranteed to not be primary
{
    // Ensure that mongos can start up when given the CSRS secondary, discover the primary, and
    // perform writes to the config servers.
    var mongos = MongoRunner.runMongos({configdb: seedList});
    var admin = mongos.getDB("admin");
    assert.commandWorked(admin.foo.insert({a: 1}));
    assert.eq(1, admin.foo.findOne().a);
    MongoRunner.stopMongos(mongos);
}

// Wait for replication to all config server replica set members to ensure that mongos
// will be able to do majority reads when trying to verify if the initial cluster metadata
// has been properly written.
rst.awaitLastOpCommitted();
// Now take down the one electable node
rst.stop(0);
rst.awaitNoPrimary();

// Start a mongos when there is no primary
var mongos = MongoRunner.runMongos({configdb: seedList});
// Take down the one node the mongos knew about to ensure that it autodiscovered the one
// remaining
// config server
rst.stop(1);

var admin = mongos.getDB("admin");
mongos.setSecondaryOk();
assert.eq(1, admin.foo.findOne().a);
MongoRunner.stopMongos(mongos);
rst.stopSet();
