/*
 * Intersperses $geoNear aggregations with updates of non-geo fields to test deduplication.
 * @tags: [
 *   requires_non_retryable_writes,
 *   requires_getmore,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/yield/yield_geo_near.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.states.remove = function remove(db, collName) {
        let id = Random.randInt(this.nDocs);
        let doc = db[collName].findOne({_id: id});
        if (doc !== null) {
            let res = db[collName].remove({_id: id});
            assert.commandWorked(res);
            if (res.nRemoved > 0) {
                // Re-insert the document with the same '_id', but an incremented
                // 'timesInserted' to
                // distinguish it from the deleted document.
                doc.timesInserted++;
                assert.commandWorked(db[collName].insert(doc));
            }
        }
    };

    /*
     * Use geo $nearSphere query to find points near the origin. Note this should be done using
     *the
     * geoNear command, rather than a $nearSphere query, as the $nearSphere query doesn't work
     *in a
     * sharded environment. Unfortunately this means we cannot batch the request.
     *
     * Only points are covered in this test as there is no guarantee that geometries indexed in
     * multiple cells will be deduplicated correctly with interspersed updates. If multiple
     *index
     * cells for the same geometry occur in the same search interval, an update may cause
     *geoNear
     * to return the same document multiple times.
     */
    $config.states.query = function geoNear(db, collName) {
        // This distance gets about 80 docs around the origin. There is one doc inserted
        // every 1m^2 and the area scanned by a 5m radius is PI*(5m)^2 ~ 79.
        const maxDistance = 5;
        const cursor = db[collName].aggregate([
            {
                $geoNear: {
                    near: [0, 0],
                    distanceField: "dist",
                    maxDistance: maxDistance,
                    spherical: true,
                },
            },
        ]);

        const seenObjs = [];
        const seenObjsOriginals = [];
        while (cursor.hasNext()) {
            const doc = cursor.next();

            // The pair (_id, timesInserted) is the smallest set of attributes that uniquely
            // identifies a document.
            const objToSearch = {_id: doc._id, timesInserted: doc.timesInserted};
            for (let i = 0; i < seenObjs.length; ++i) {
                assert.neq(
                    bsonWoCompare(seenObjs[i], objToSearch),
                    0,
                    () =>
                        `$geoNear returned document ${tojson(doc)} multiple ` +
                        `times: first occurence was ${tojson(seenObjsOriginals[i])}`,
                );
            }
            seenObjs.push(objToSearch);
            seenObjsOriginals.push(doc);
        }
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        // Attempts to perform an in-place update to trigger an invalidation on MMAP v1.
        return {$inc: {timesUpdated: 1}};
    };

    $config.data.getIndexSpec = function getIndexSpec() {
        return {geo: "2dsphere"};
    };

    $config.data.getReplaceSpec = function getReplaceSpec(i, coords) {
        return {_id: i, geo: coords, timesUpdated: 0, timesInserted: 0};
    };

    return $config;
});
