// SERVER-7906

var coll = 'points'

function test(db, sharded, indexType) {
    printjson(db);
    db[coll].drop();

    if (sharded) {
        var shards = []
        var config = shardedDB.getSiblingDB("config");
        config.shards.find().forEach(function(shard) { shards.push(shard._id) });

        shardedDB.adminCommand({shardCollection: shardedDB[coll].getFullName(), key: {rand:1}});
        for (var i=1; i < 10; i++) {
            // split at 0.1, 0.2, ... 0.9
            shardedDB.adminCommand({split: shardedDB[coll].getFullName(), middle: {rand: i/10}});
            shardedDB.adminCommand({moveChunk: shardedDB[coll].getFullName(), find: {rand: i/10},
                            to: shards[i%shards.length]});
        }

        assert.eq(config.chunks.count({'ns': shardedDB[coll].getFullName()}), 10);
    }

    var numPts = 10*1000;
    for (var i=0; i < numPts; i++) {
        var lat = 90 - Random.rand() * 180;
        var lng = 180 - Random.rand() * 360;
        db[coll].insert({rand:Math.random(), loc: [lat, lng]})
    }
    db.getLastError();
    assert.eq(db[coll].count(), numPts);

    db[coll].ensureIndex({loc: indexType})

    var queryPoint = [0,0]
    geoCmd = {geoNear: coll, near: queryPoint, spherical: true, includeLocs: true};
    assert.commandWorked(db.runCommand(geoCmd), tojson({sharded: sharded, indexType: indexType}));
}

var sharded = new ShardingTest({shards: 3, verbose: 0, mongos: 1});
sharded.stopBalancer();
sharded.adminCommand( { enablesharding : "test" } );
var shardedDB = sharded.getDB('test');
printjson(shardedDB);

test(shardedDB, true, '2dsphere');
sharded.stop();
