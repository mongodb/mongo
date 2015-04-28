// Test that the TTL monitor will correctly use TTL indexes that are also
// partial indexes.  SERVER-17984 (test disabled until SERVER-17984 is resolved).
/*
(function() {
    "use strict";
    // Launch mongod with shorter TTL monitor sleep interval.
    var runner = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1"});
    var coll = runner.getDB("test").ttl_partial_index;
    coll.drop();

    // Create TTL partial index.
    assert.commandWorked(coll.ensureIndex({x: 1}, {expireAfterSeconds: 0,
                                                   filter: {z: {$exists: true}}}));

    var now = (new Date()).getTime();
    assert.writeOK(coll.insert({x: now, z: 2}));
    assert.writeOK(coll.insert({x: now}));

    // Wait for the TTL monitor to run.
    var ttlPass = coll.getDB().serverStatus().metrics.ttl.passes;
    assert.soon(function() {
                    return ttlPass < coll.getDB().serverStatus().metrics.ttl.passes;
                },
                "TTL monitor didn't run before timing out.");

    assert.eq(0, coll.find({z: {$exists: 1}}).hint({x: 1}).itcount(),
              "Wrong number of documents in partial index, after TTL monitor run");
    assert.eq(1, coll.find().itcount(),
              "Wrong number of documents in collection, after TTL monitor run");
})();
*/
