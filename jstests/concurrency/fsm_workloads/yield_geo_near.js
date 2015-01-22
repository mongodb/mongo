'use strict';

/*
 * yield_geo_near.js (extends yield_text.js)
 *
 * Intersperse geo $near queries with updates and deletes of documents they may match.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js'); // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield_text.js'); // for $config

var $config = extendWorkload($config, function($config, $super) {

    /*
     * Use geo $near query to find points near the origin. Note this should be done using the
     * geoNear command, rather than a $near query, as the $near query doesn't work in a sharded
     * environment. Unfortunately this means we cannot batch the request.
     */
    $config.states.query = function geoNear(db, collName) {
        // This distance gets about 80 docs around the origin. Don't ask why, too lazy to do
        // the math.
        var maxDistance = 5;

        var res = db.runCommand({ geoNear: collName, near: [0, 0], maxDistance: maxDistance });
        assertWhenOwnColl.commandWorked(res);  // Could fail if more than 1 2d index.
        assertWhenOwnColl(function verifyResults() {
            var results = res.results;
            var prevDoc = { dis: -Infinity };
            for (var i = 0; i < results.length; i++) {
                var doc = results[i];
                assertAlways.lte(NumberInt(doc.dis), maxDistance);  // satisfies query
                assertAlways.lte(prevDoc.dis, doc.dis);  // returned in the correct order
            }
        });
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        var P = Math.floor(Math.sqrt($config.data.nDocs));
        var newX = Random.randInt(P);
        var newY = Random.randInt(P);
        return { $set: { geo: [newX, newY] } };
    };

    /*
     * Insert some docs in geo form and make a 2d index.
     */
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        var P = Math.floor(Math.sqrt($config.data.nDocs));
        var i = this.nDocs;
        // Set up some points to query (in a PxP grid around 0,0).
        var bulk = db[collName].initializeUnorderedBulkOp();
        for (var x = -P; x < P; x++) {
            for (var y = -P; y < P; y++) {
                bulk.find({ _id: i }).upsert().replaceOne({ _id: i, geo: [x,y] });
                i++;
            }
        }
        assertAlways.writeOK(bulk.execute());
        assertAlways.commandWorked(db[collName].ensureIndex({ geo: '2d' }));
    };

    return $config;
});
