/**
 * Tests that a variety of operations from a mongos to a shard succeed even during the period when
 * the shard has yet to receive the shardIdentity from the config server.
 */
(function() {
    'use strict';

    // Simulate that the insert of the shardIdentity doc from the config to a new shard gets
    // "delayed" by using the dontUpsertShardIdentityOnNewShards failpoint on the configs.
    var st = new ShardingTest({
        shards: 3,
        mongos: 1,
        other: {
            rs: true,
            rsOptions: {nodes: 1},
            configOptions: {
                setParameter: "failpoint.dontUpsertShardIdentityOnNewShards={'mode':'alwaysOn'}"
            }
        }
    });

    var testDB = st.s.getDB("test");
    assert.commandWorked(testDB.adminCommand({enableSharding: testDB.getName()}));
    st.ensurePrimaryShard(testDB.getName(), st.shard0.shardName);

    // Create a collection sharded on {a: 1}. Add 2dsphere index to test geoNear.
    var coll = testDB.getCollection("sharded");
    coll.drop();
    assert.commandWorked(coll.createIndex({a: 1}));
    assert.commandWorked(coll.createIndex({geo: "2dsphere"}));
    assert.commandWorked(testDB.adminCommand({shardCollection: coll.getFullName(), key: {a: 1}}));

    // Split the collection.
    // shard0000: { "a" : { "$minKey" : 1 } } -->> { "a" : 1 }
    // shard0001: { "a" : 1 } -->> { "a" : 10 }
    // shard0002: { "a" : 10 } -->> { "a" : { "$maxKey" : 1 }}
    var chunk2Min = 1;
    var chunk3Min = 10;
    assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {a: chunk2Min}}));
    assert.commandWorked(testDB.adminCommand({split: coll.getFullName(), middle: {a: chunk3Min}}));
    assert.commandWorked(testDB.adminCommand(
        {moveChunk: coll.getFullName(), find: {a: 5}, to: st.shard1.shardName}));
    assert.commandWorked(testDB.adminCommand(
        {moveChunk: coll.getFullName(), find: {a: 15}, to: st.shard2.shardName}));

    // Put data on each shard.
    // Note that the balancer is off by default, so the chunks will stay put.
    // shard0000: {a: 0}
    // shard0001: {a: 2}, {a: 4}
    // shard0002: {a: 15}
    // Include geo field to test geoNear.
    var a_0 = {_id: 0, a: 0, geo: {type: "Point", coordinates: [0, 0]}};
    var a_2 = {_id: 1, a: 2, geo: {type: "Point", coordinates: [0, 0]}};
    var a_4 = {_id: 2, a: 4, geo: {type: "Point", coordinates: [0, 0]}};
    var a_15 = {_id: 3, a: 15, geo: {type: "Point", coordinates: [0, 0]}};
    assert.writeOK(coll.insert(a_0));
    assert.writeOK(coll.insert(a_2));
    assert.writeOK(coll.insert(a_4));
    assert.writeOK(coll.insert(a_15));

    // Aggregate and aggregate explain.
    assert.eq(3, coll.aggregate([{$match: {a: {$lt: chunk3Min}}}]).itcount());
    assert.commandWorked(coll.explain().aggregate([{$match: {a: {$lt: chunk3Min}}}]));

    // Count and count explain.
    assert.eq(3, coll.find({a: {$lt: chunk3Min}}).count());
    assert.commandWorked(coll.explain().find({a: {$lt: chunk3Min}}).count());

    // Distinct and distinct explain.
    assert.eq(3, coll.distinct("_id", {a: {$lt: chunk3Min}}).length);
    assert.commandWorked(coll.explain().distinct("_id", {a: {$lt: chunk3Min}}));

    // Find and find explain.
    assert.eq(3, coll.find({a: {$lt: chunk3Min}}).itcount());
    assert.commandWorked(coll.find({a: {$lt: chunk3Min}}).explain());

    // FindAndModify and findAndModify explain.
    assert.eq(0, coll.findAndModify({query: {a: 0}, update: {$set: {b: 1}}}).a);
    assert.commandWorked(coll.explain().findAndModify({query: {a: 0}, update: {$set: {b: 1}}}));

    // GeoNear.
    assert.eq(3,
              assert
                  .commandWorked(testDB.runCommand({
                      geoNear: coll.getName(),
                      near: {type: "Point", coordinates: [0, 0]},
                      spherical: true,
                      query: {a: {$lt: chunk3Min}},
                  }))
                  .results.length);

    // MapReduce.
    assert.eq(3,
              assert
                  .commandWorked(coll.mapReduce(
                      function() {
                          emit(this.a, 1);
                      },
                      function(key, values) {
                          return Array.sum(values);
                      },
                      {out: {inline: 1}, query: {a: {$lt: chunk3Min}}}))
                  .results.length);

    // Remove and remove explain.
    var writeRes = coll.remove({a: {$lt: chunk3Min}});
    assert.writeOK(writeRes);
    assert.eq(3, writeRes.nRemoved);
    assert.commandWorked(coll.explain().remove({a: {$lt: chunk3Min}}));
    assert.writeOK(coll.insert(a_0));
    assert.writeOK(coll.insert(a_2));
    assert.writeOK(coll.insert(a_4));

    // Update and update explain.
    writeRes = coll.update({a: {$lt: chunk3Min}}, {$set: {b: 1}}, {multi: true});
    assert.writeOK(writeRes);
    assert.eq(3, writeRes.nMatched);
    assert.commandWorked(
        coll.explain().update({a: {$lt: chunk3Min}}, {$set: {b: 1}}, {multi: true}));

    // Assert that the shardIdentity document has still not "reached" any shard, meaning all of the
    // above commands indeed succeeded during the period that the shardIdentity insert was
    // "delayed."
    for (shard in st.shards) {
        var res = shard.getDB("admin").getCollection("system.version").find({_id: "shardIdentity"});
        assert.eq(null, res);
    }

    st.stop();

})();
