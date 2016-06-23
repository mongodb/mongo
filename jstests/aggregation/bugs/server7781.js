// SERVER-7781 $geoNear pipeline stage
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

    function checkOutput(cmdOut, aggOut, expectedNum) {
        assert.commandWorked(cmdOut, "geoNear command");

        // the output arrays are accessed differently
        cmdOut = cmdOut.results;
        aggOut = aggOut.toArray();

        assert.eq(cmdOut.length, expectedNum);
        assert.eq(aggOut.length, expectedNum);

        var allSame = true;
        var massaged;  // massage geoNear command output to match output from agg pipeline
        for (var i = 0; i < cmdOut.length; i++) {
            massaged = {};
            Object.extend(massaged, cmdOut[i].obj, /*deep=*/true);
            massaged.stats = {'dis': cmdOut[i].dis, 'loc': cmdOut[i].loc};

            if (!friendlyEqual(massaged, aggOut[i])) {
                allSame = false;  // don't bail yet since we want to print all differences
                print("Difference detected at index " + i + " of " + expectedNum);
                print("from geoNear command:" + tojson(massaged));
                print("from aggregate command:" + tojson(aggOut[i]));
            }
        }

        assert(allSame);
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

        // test with defaults
        var queryPoint = pointMaker.mkPt(0.25);  // stick to center of map
        var geoCmd = {geoNear: coll, near: queryPoint, includeLocs: true, spherical: true};
        var aggCmd = {
            $geoNear: {
                near: queryPoint,
                includeLocs: 'stats.loc',
                distanceField: 'stats.dis',
                spherical: true
            }
        };
        checkOutput(db.runCommand(geoCmd), db[coll].aggregate(aggCmd), 100);

        // test with num
        queryPoint = pointMaker.mkPt(0.25);
        geoCmd.num = 75;
        geoCmd.near = queryPoint;
        aggCmd.$geoNear.num = 75;
        aggCmd.$geoNear.near = queryPoint;
        checkOutput(db.runCommand(geoCmd), db[coll].aggregate(aggCmd), 75);

        // test with limit instead of num (they mean the same thing, but want to test both)
        queryPoint = pointMaker.mkPt(0.25);
        geoCmd.near = queryPoint;
        delete geoCmd.num;
        geoCmd.limit = 70;
        aggCmd.$geoNear.near = queryPoint;
        delete aggCmd.$geoNear.num;
        aggCmd.$geoNear.limit = 70;
        checkOutput(db.runCommand(geoCmd), db[coll].aggregate(aggCmd), 70);

        // test spherical
        queryPoint = pointMaker.mkPt(0.25);
        geoCmd.spherical = true;
        geoCmd.near = queryPoint;
        aggCmd.$geoNear.spherical = true;
        aggCmd.$geoNear.near = queryPoint;
        checkOutput(db.runCommand(geoCmd), db[coll].aggregate(aggCmd), 70);

        // test $geoNear + $limit coalescing
        queryPoint = pointMaker.mkPt(0.25);
        geoCmd.num = 40;
        geoCmd.near = queryPoint;
        aggCmd.$geoNear.near = queryPoint;
        var aggArr = [aggCmd, {$limit: 50}, {$limit: 60}, {$limit: 40}];
        checkOutput(db.runCommand(geoCmd), db[coll].aggregate(aggArr), 40);

        // Test $geoNear with an initial batchSize of 0. Regression test for SERVER-20935.
        queryPoint = pointMaker.mkPt(0.25);
        geoCmd.spherical = true;
        geoCmd.near = queryPoint;
        geoCmd.limit = 70;
        delete geoCmd.num;
        aggCmd.$geoNear.spherical = true;
        aggCmd.$geoNear.near = queryPoint;
        aggCmd.$geoNear.limit = 70;
        delete aggCmd.$geoNear.num;
        var cmdRes = db[coll].runCommand("aggregate", {pipeline: [aggCmd], cursor: {batchSize: 0}});
        assert.commandWorked(cmdRes);
        var cmdCursor = new DBCommandCursor(db[coll].getMongo(), cmdRes, 0);
        checkOutput(db.runCommand(geoCmd), cmdCursor, 70);
    }

    test(db, false, '2d');
    test(db, false, '2dsphere');

    var sharded = new ShardingTest({shards: 3, mongos: 1});
    assert.commandWorked(sharded.s0.adminCommand({enablesharding: "test"}));
    sharded.ensurePrimaryShard('test', 'shard0001');

    test(sharded.getDB('test'), true, '2d');
    test(sharded.getDB('test'), true, '2dsphere');

    sharded.stop();
})();
