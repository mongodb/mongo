// Tests that mongos can autodiscover a config server replica set when the only node it knows about
// is not the primary.

(function() {

    'use strict';

    var rst = new ReplSetTest(
        {name: "configRS", nodes: 3, nodeOptions: {configsvr: "", storageEngine: "wiredTiger"}});
    rst.startSet();
    var conf = rst.getReplSetConfig();
    conf.members[1].priority = 0;
    conf.members[2].priority = 0;
    conf.writeConcernMajorityJournalDefault = true;
    rst.initiate(conf);

    var seedList = rst.name + "/" + rst.nodes[1].host;  // node 1 is guaranteed to not be primary
    {
        // Ensure that mongos can start up when given the CSRS secondary, discover the primary, and
        // perform writes to the config servers.
        var mongos = MongoRunner.runMongos({configdb: seedList});
        var admin = mongos.getDB('admin');
        assert.writeOK(admin.foo.insert({a: 1}));
        assert.eq(1, admin.foo.findOne().a);
        MongoRunner.stopMongos(mongos);
    }

    // Wait for replication to all config server replica set members to ensure that mongos
    // doesn't read from a stale config server when trying to verify if the initial cluster metadata
    // has been properly written.
    rst.awaitReplication();
    // Now take down the one electable node
    rst.stop(0);
    rst.awaitNoPrimary();

    // Start a mongos when there is no primary
    var mongos = MongoRunner.runMongos({configdb: seedList});
    // Take down the one node the mongos knew about to ensure that it autodiscovered the one
    // remaining
    // config server
    rst.stop(1);

    var admin = mongos.getDB('admin');
    mongos.setSlaveOk(true);
    assert.eq(1, admin.foo.findOne().a);

})();
