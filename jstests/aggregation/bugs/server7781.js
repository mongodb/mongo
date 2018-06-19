// SERVER-7781 $geoNear pipeline stage
// @tags: [requires_sharding]
(function() {
    'use strict';

    load('jstests/libs/geo_near_random.js');
    load('jstests/aggregation/extras/utils.js');

    var coll = 'server7781';

    db[coll].drop();
    db[coll].insert({loc: [0, 0]});

    // $geoNear is only allowed as the first stage in a pipeline, nowhere else.
    assert.throws(
        () => db[coll].aggregate(
            [{$match: {x: 1}}, {$geoNear: {near: [1, 1], spherical: true, distanceField: 'dis'}}]));

    const kDistanceField = "dis";
    const kIncludeLocsField = "loc";

    /**
     * Tests the output of the $geoNear command. This function expects a document with the following
     * fields:
     *   - 'geoNearSpec' is the specification for a $geoNear aggregation stage.
     *   - 'limit' is an integer limiting the number of pipeline results.
     *   - 'batchSize', if specified, is the batchSize to use for the aggregation.
     */
    function testGeoNearStageOutput({geoNearSpec, limit, batchSize}) {
        const aggOptions = batchSize ? {batchSize: batchSize} : {};
        const result =
            db[coll].aggregate([{$geoNear: geoNearSpec}, {$limit: limit}], aggOptions).toArray();
        const errmsg = () => tojson(result);

        // Verify that we got the expected number of results.
        assert.eq(result.length, limit, errmsg);

        // Run though the array, checking for proper sort order and sane computed distances.
        result.reduce((lastDist, curDoc) => {
            const curDist = curDoc[kDistanceField];

            // Verify that distances are in increasing order.
            assert.lte(lastDist, curDist, errmsg);

            // Verify that the computed distance is correct.
            const computed = Geo.sphereDistance(geoNearSpec["near"], curDoc[kIncludeLocsField]);
            assert.close(computed, curDist, errmsg);
            return curDist;
        }, 0);
    }

    // We use this to generate points. Using a single global to avoid reseting RNG in each pass.
    var pointMaker = new GeoNearRandomTest(coll);

    function test(db, sharded, indexType) {
        db[coll].drop();

        if (sharded) {  // sharded setup
            var shards = [];
            var config = db.getSiblingDB("config");
            config.shards.find().forEach(function(shard) {
                shards.push(shard._id);
            });

            assert.commandWorked(
                db.adminCommand({shardCollection: db[coll].getFullName(), key: {rand: 1}}));
            for (var i = 1; i < 10; i++) {
                // split at 0.1, 0.2, ... 0.9
                assert.commandWorked(
                    db.adminCommand({split: db[coll].getFullName(), middle: {rand: i / 10}}));
                db.adminCommand({
                    moveChunk: db[coll].getFullName(),
                    find: {rand: i / 10},
                    to: shards[i % shards.length]
                });
            }

            assert.eq(config.chunks.count({'ns': db[coll].getFullName()}), 10);
        }

        // insert points
        var numPts = 10 * 1000;
        var bulk = db[coll].initializeUnorderedBulkOp();
        for (var i = 0; i < numPts; i++) {
            bulk.insert({rand: Math.random(), loc: pointMaker.mkPt()});
        }
        assert.writeOK(bulk.execute());

        assert.eq(db[coll].count(), numPts);

        db[coll].ensureIndex({loc: indexType});

        // Test $geoNear with spherical coordinates.
        testGeoNearStageOutput({
            geoNearSpec: {
                near: pointMaker.mkPt(0.25),
                distanceField: kDistanceField,
                includeLocs: kIncludeLocsField,
                spherical: true,
            },
            limit: 100
        });

        // Test $geoNear with an initial batchSize of 1.
        testGeoNearStageOutput({
            geoNearSpec: {
                near: pointMaker.mkPt(0.25),
                distanceField: kDistanceField,
                includeLocs: kIncludeLocsField,
                spherical: true,
            },
            limit: 70,
            batchSize: 1
        });
    }

    test(db, false, '2d');
    test(db, false, '2dsphere');

    var sharded = new ShardingTest({shards: 3, mongos: 1});
    assert.commandWorked(sharded.s0.adminCommand({enablesharding: "test"}));
    sharded.ensurePrimaryShard('test', sharded.shard1.shardName);

    test(sharded.getDB('test'), true, '2d');
    test(sharded.getDB('test'), true, '2dsphere');

    sharded.stop();
})();
