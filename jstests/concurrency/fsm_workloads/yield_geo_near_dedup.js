'use strict';

/*
 * yield_geo_near_dedup.js (extends yield_geo_near.js)
 *
 * Intersperse geo $near queries with updates of non-geo fields to test deduplication.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js'); // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield_geo_near.js'); // for $config

var $config = extendWorkload($config, function($config, $super) {

    /*
     * Use geo $nearSphere query to find points near the origin. Note this should be done using the
     * geoNear command, rather than a $nearSphere query, as the $nearSphere query doesn't work in a
     * sharded environment. Unfortunately this means we cannot batch the request.
     *
     * Only points are covered in this test as there is no guarantee that geometries indexed in
     * multiple cells will be deduplicated correctly with interspersed updates. If multiple index
     * cells for the same geometry occur in the same search interval, an update may cause geoNear
     * to return the same document multiple times.
     */
    $config.states.query = function geoNear(db, collName) {
        // This distance gets about 80 docs around the origin. There is one doc inserted
        // every 1m^2 and the area scanned by a 5m radius is PI*(5m)^2 ~ 79.
        var maxDistance = 5;

        var res = db.runCommand({
            geoNear: collName,
            near: [0, 0],
            maxDistance: maxDistance,
            spherical: true
        });
        assertWhenOwnColl.commandWorked(res);
        assertWhenOwnColl(function verifyResults() {
            var results = res.results;
            // TODO: Support non-primitive _id
            var ids = [];
            for (var i = 0; i < results.length; i++) {
                var id = results[i].obj._id;
                assertWhenOwnColl.eq(-1, ids.indexOf(id),
                    'geoNear command returned a duplicate id of [ ' + id + ' ]: ' + tojson(ids));
                ids.push(id);
            }
        });
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        // Attempts to perform an in-place update to trigger an invalidation on MMAP v1.
        return { $inc: { num: 1 } };
    };

    $config.data.getIndexSpec = function getIndexSpec() {
        return { geo: '2dsphere' };
    }

    $config.data.getReplaceSpec = function getReplaceSpec(i, coords) {
        return { _id: i, geo: coords, num: 0 };
    }

    return $config;
});
