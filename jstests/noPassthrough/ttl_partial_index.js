// Test that the TTL monitor will correctly use TTL indexes that are also partial indexes.
// SERVER-17984.
(function() {
    "use strict";
    // Launch mongod with shorter TTL monitor sleep interval.
    var runner = MongoRunner.runMongod({setParameter: "ttlMonitorSleepSecs=1"});
    var coll = runner.getDB("test").ttl_partial_index;
    coll.drop();

    // Create TTL partial index.
    assert.commandWorked(coll.ensureIndex(
        {x: 1}, {expireAfterSeconds: 0, partialFilterExpression: {z: {$exists: true}}}));

    var now = new Date();
    assert.writeOK(coll.insert({x: now, z: 2}));
    assert.writeOK(coll.insert({x: now}));

    // Wait for the TTL monitor to run at least twice (in case we weren't finished setting up our
    // collection when it ran the first time).
    var ttlPass = coll.getDB().serverStatus().metrics.ttl.passes;
    assert.soon(function() {
        return coll.getDB().serverStatus().metrics.ttl.passes >= ttlPass + 2;
    }, "TTL monitor didn't run before timing out.");

    assert.eq(0,
              coll.find({z: {$exists: true}}).hint({x: 1}).itcount(),
              "Wrong number of documents in partial index, after TTL monitor run");
    assert.eq(
        1, coll.find().itcount(), "Wrong number of documents in collection, after TTL monitor run");
})();
