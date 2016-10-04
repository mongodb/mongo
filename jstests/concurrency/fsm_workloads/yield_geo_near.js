'use strict';

/*
 * yield_geo_near.js (extends yield.js)
 *
 * Intersperse geo $near queries with updates and deletes of documents they may match.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield.js');       // for $config

var $config = extendWorkload($config, function($config, $super) {

    /*
     * Use geo $near query to find points near the origin. Note this should be done using the
     * geoNear command, rather than a $near query, as the $near query doesn't work in a sharded
     * environment. Unfortunately this means we cannot batch the request.
     */
    $config.states.query = function geoNear(db, collName) {
        // This distance gets about 80 docs around the origin. There is one doc inserted
        // every 1m^2 and the area scanned by a 5m radius is PI*(5m)^2 ~ 79.
        var maxDistance = 5;

        var res = db.runCommand({geoNear: collName, near: [0, 0], maxDistance: maxDistance});
        assertWhenOwnColl.commandWorked(res);  // Could fail if more than 1 2d index.
        assertWhenOwnColl(function verifyResults() {
            var results = res.results;
            var prevDoc = {dis: 0};  // distance should never be less than 0
            for (var i = 0; i < results.length; i++) {
                var doc = results[i];
                assertAlways.lte(NumberInt(doc.dis), maxDistance);  // satisfies query
                assertAlways.lte(prevDoc.dis, doc.dis);             // returned in the correct order
                prevDoc = doc;
            }
        });
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        var P = Math.floor(Math.sqrt(this.nDocs));

        // Move the point to another location within the PxP grid.
        var newX = Random.randInt(P) - P / 2;
        var newY = Random.randInt(P) - P / 2;
        return {$set: {geo: [newX, newY]}};
    };

    $config.data.getIndexSpec = function getIndexSpec() {
        return {geo: '2d'};
    };

    $config.data.getReplaceSpec = function getReplaceSpec(i, coords) {
        return {_id: i, geo: coords};
    };

    /*
     * Insert some docs in geo form and make a 2d index.
     */
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        var P = Math.floor(Math.sqrt(this.nDocs));
        var i = 0;
        // Set up some points to query (in a PxP grid around 0,0).
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var x = 0; x < P; x++) {
            for (var y = 0; y < P; y++) {
                var coords = [x - P / 2, y - P / 2];
                bulk.find({_id: i}).upsert().replaceOne(this.getReplaceSpec(i, coords));
                i++;
            }
        }
        assertAlways.writeOK(bulk.execute());
        assertAlways.commandWorked(db[collName].ensureIndex(this.getIndexSpec()));
    };

    return $config;
});
