// SERVER-7906
load('jstests/libs/geo_near_random.js');

var coll = 'points'

function test(db, sharded, indexType) {
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

    var pointMaker = new GeoNearRandomTest(coll);
    var numPts = 10*1000;
    for (var i=0; i < numPts; i++) {
        db[coll].insert({rand:Math.random(), loc: pointMaker.mkPt()})
    }
    db.getLastError();
    assert.eq(db[coll].count(), numPts);

    db[coll].ensureIndex({loc: indexType})

    var queryPoint = pointMaker.mkPt(0.25) // stick to center of map
    geoCmd = {geoNear: coll, near: queryPoint, includeLocs: true};
    assert.commandWorked(db.runCommand(geoCmd), tojson({sharded: sharded, indexType: indexType}));
}

var sharded = new ShardingTest({shards: 3, verbose: 0, mongos: 1});
sharded.stopBalancer();
sharded.adminCommand( { enablesharding : "test" } );
var shardedDB = sharded.getDB('test');


test(db, false, '2d');
test(db, false, '2dsphere');
test(shardedDB, true, '2d');
test(shardedDB, true, '2dsphere');
sharded.stop();
