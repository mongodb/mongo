'use strict';

/*
 * Intersperses $geoNear aggregations with updates and deletes of documents they may match.
 * @tags: [requires_non_retryable_writes]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield.js');       // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.states.query = function geoNear(db, collName) {
        // This distance gets about 80 docs around the origin. There is one doc inserted
        // every 1m^2 and the area scanned by a 5m radius is PI*(5m)^2 ~ 79.
        const maxDistance = 5;
        const cursor = db[collName].aggregate([{
            $geoNear: {
                near: [0, 0],
                distanceField: "dist",
                maxDistance: maxDistance,
            }
        }]);

        // We only run the verification when workloads are run on separate collections, since the
        // aggregation may fail if we don't have exactly one 2d index to use.
        assertWhenOwnColl(function verifyResults() {
            // We manually verify the results ourselves rather than calling advanceCursor(). In the
            // event of a failure, the aggregation cursor cannot support explain().
            let lastDistanceSeen = 0;
            while (cursor.hasNext()) {
                const doc = cursor.next();
                assertAlways.lte(doc.dist,
                                 maxDistance,
                                 `dist in ${tojson(doc)} exceeds max allowable $geoNear distance`);
                assertAlways.lte(lastDistanceSeen,
                                 doc.dist,
                                 `dist in ${tojson(doc)} is not less than the previous distance`);
                lastDistanceSeen = doc.dist;
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
