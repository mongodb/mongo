(function() {

    // Don't start any shards, yet
    var s =
        new ShardingTest({name: "add_shard2", shards: 1, mongos: 1, other: {useHostname: true}});

    // Start two new instances, which will be used for shards
    var conn1 = MongoRunner.runMongod({useHostname: true});
    var conn2 = MongoRunner.runMongod({useHostname: true});

    var rs1 = new ReplSetTest({"name": "add_shard2_rs1", nodes: 3});
    rs1.startSet();
    rs1.initiate();
    var master1 = rs1.getPrimary();

    var rs2 = new ReplSetTest({"name": "add_shard2_rs2", nodes: 3});
    rs2.startSet();
    rs2.initiate();
    var master2 = rs2.getPrimary();

    // replica set with set name = 'config'
    var rs3 = new ReplSetTest({'name': 'config', nodes: 3});
    rs3.startSet();
    rs3.initiate();

    // replica set with set name = 'admin'
    var rs4 = new ReplSetTest({'name': 'admin', nodes: 3});
    rs4.startSet();
    rs4.initiate();

    // replica set with configsvr: true should *not* be allowed to be added as a shard
    var rs5 = new ReplSetTest({
        name: 'csrs',
        nodes: 3,
        nodeOptions: {configsvr: "", journal: "", storageEngine: "wiredTiger"}
    });
    rs5.startSet();
    var conf = rs5.getReplSetConfig();
    conf.configsvr = true;
    rs5.initiate(conf);

    // step 1. name given. maxSize zero means no limit. Make sure it is allowed.
    assert.commandWorked(
        s.admin.runCommand({addshard: getHostName() + ":" + conn1.port, name: "bar", maxSize: 0}));
    var shard = s.getDB("config").shards.findOne({"_id": {"$nin": ["shard0000"]}});
    assert(shard, "shard wasn't found");
    assert.eq("bar", shard._id, "shard has incorrect name");

    // step 2. replica set
    assert(
        s.admin.runCommand({"addshard": "add_shard2_rs1/" + getHostName() + ":" + master1.port}).ok,
        "failed to add shard in step 2");
    shard = s.getDB("config").shards.findOne({"_id": {"$nin": ["shard0000", "bar"]}});
    assert(shard, "shard wasn't found");
    assert.eq("add_shard2_rs1", shard._id, "t2 name");

    // step 3. replica set w/ name given
    assert(s.admin
               .runCommand({
                   "addshard": "add_shard2_rs2/" + getHostName() + ":" + master2.port,
                   "name": "myshard"
               })
               .ok,
           "failed to add shard in step 4");
    shard =
        s.getDB("config").shards.findOne({"_id": {"$nin": ["shard0000", "bar", "add_shard2_rs1"]}});
    assert(shard, "shard wasn't found");
    assert.eq("myshard", shard._id, "t3 name");

    // step 4. no name given
    assert(s.admin.runCommand({"addshard": getHostName() + ":" + conn2.port}).ok,
           "failed to add shard in step 4");
    shard = s.getDB("config").shards.findOne(
        {"_id": {"$nin": ["shard0000", "bar", "add_shard2_rs1", "myshard"]}});
    assert(shard, "shard wasn't found");
    assert.eq("shard0001", shard._id, "t4 name");

    assert.eq(s.getDB("config").shards.count(), 5, "unexpected number of shards");

    // step 5. replica set w/ a wrong host
    var portWithoutHostRunning = allocatePort();
    assert(
        !s.admin.runCommand({addshard: "add_shard2_rs2/NonExistingHost:" + portWithoutHostRunning})
             .ok,
        "accepted bad hostname in step 5");

    // step 6. replica set w/ mixed wrong/right hosts
    assert(!s.admin
                .runCommand({
                    addshard: "add_shard2_rs2/" + getHostName() + ":" + master2.port + ",foo:" +
                        portWithoutHostRunning
                })
                .ok,
           "accepted bad hostname in step 6");

    // Cannot add invalid stand alone host.
    assert.commandFailed(s.admin.runCommand({addshard: 'dummy:12345'}));

    //
    // SERVER-17231 Adding replica set w/ set name = 'config'
    //
    var configReplURI = 'config/' + getHostName() + ':' + rs3.getPrimary().port;

    assert(!s.admin.runCommand({'addshard': configReplURI}).ok,
           'accepted replica set shard with set name "config"');
    // but we should be allowed to add that replica set using a different shard name
    assert(s.admin.runCommand({'addshard': configReplURI, name: 'not_config'}).ok,
           'unable to add replica set using valid replica set name');

    shard = s.getDB('config').shards.findOne({'_id': 'not_config'});
    assert(shard, 'shard with name "not_config" not found');

    //
    // SERVER-17232 Try inserting into shard with name 'admin'
    //
    assert(
        s.admin.runCommand({'addshard': 'admin/' + getHostName() + ':' + rs4.getPrimary().port}).ok,
        'adding replica set with name "admin" should work');
    var wRes = s.getDB('test').foo.insert({x: 1});
    assert(!wRes.hasWriteError() && wRes.nInserted === 1,
           'failed to insert document into "test.foo" unsharded collection');

    // SERVER-19545 Should not be able to add config server replsets as shards.
    assert.commandFailed(s.admin.runCommand({addshard: rs5.getURL()}));

    s.stop();

    rs1.stopSet();
    rs2.stopSet();
    rs3.stopSet();
    rs4.stopSet();
    rs5.stopSet();

})();
