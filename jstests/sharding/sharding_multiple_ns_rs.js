// When checking UUID consistency, the shell attempts to run a command on the node it believes is
// primary in each shard. However, this test shuts down the primary of the shard. Since whether or
// not the shell detects the new primary before issuing the command is nondeterministic, skip the
// consistency check for this test.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;

(function() {
    'use strict';

    load("jstests/replsets/rslib.js");

    var s = new ShardingTest({shards: 1, mongos: 1, other: {rs: true, chunkSize: 1}});

    assert.commandWorked(s.s0.adminCommand({enablesharding: "test"}));
    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.foo", key: {_id: 1}}));

    var db = s.getDB("test");

    var bulk = db.foo.initializeUnorderedBulkOp();
    var bulk2 = db.bar.initializeUnorderedBulkOp();
    for (var i = 0; i < 100; i++) {
        bulk.insert({_id: i, x: i});
        bulk2.insert({_id: i, x: i});
    }
    assert.writeOK(bulk.execute());
    assert.writeOK(bulk2.execute());

    s.splitAt("test.foo", {_id: 50});

    var other = new Mongo(s.s0.name);
    var dbother = other.getDB("test");

    assert.eq(5, db.foo.findOne({_id: 5}).x);
    assert.eq(5, dbother.foo.findOne({_id: 5}).x);

    assert.eq(5, db.bar.findOne({_id: 5}).x);
    assert.eq(5, dbother.bar.findOne({_id: 5}).x);

    s.rs0.awaitReplication();
    s.rs0.stopMaster(15);

    // Wait for the primary to come back online...
    var primary = s.rs0.getPrimary();

    // Wait for the mongos to recognize the new primary...
    awaitRSClientHosts(db.getMongo(), primary, {ismaster: true});

    assert.eq(5, db.foo.findOne({_id: 5}).x);
    assert.eq(5, db.bar.findOne({_id: 5}).x);

    assert.commandWorked(s.s0.adminCommand({shardcollection: "test.bar", key: {_id: 1}}));
    s.splitAt("test.bar", {_id: 50});

    var yetagain = new Mongo(s.s.name);
    assert.eq(5, yetagain.getDB("test").bar.findOne({_id: 5}).x);
    assert.eq(5, yetagain.getDB("test").foo.findOne({_id: 5}).x);

    assert.eq(5, dbother.bar.findOne({_id: 5}).x);
    assert.eq(5, dbother.foo.findOne({_id: 5}).x);

    s.stop();
})();
