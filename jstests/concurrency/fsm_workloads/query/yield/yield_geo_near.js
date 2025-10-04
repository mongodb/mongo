/*
 * Intersperses $geoNear aggregations with updates and deletes of documents they may match.
 * @tags: [
 *   requires_non_retryable_writes,
 *   requires_getmore,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/yield/yield.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
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
                },
            },
        ]);

        // We only run the verification when workloads are run on separate collections, since the
        // aggregation may fail if we don't have exactly one 2d index to use.
        // We manually verify the results ourselves rather than calling advanceCursor(). In the
        // event of a failure, the aggregation cursor cannot support explain().
        let lastDistanceSeen = 0;
        while (cursor.hasNext()) {
            const doc = cursor.next();
            assert.lte(doc.dist, maxDistance, `dist in ${tojson(doc)} exceeds max allowable $geoNear distance`);
            assert.lte(lastDistanceSeen, doc.dist, `dist in ${tojson(doc)} is not less than the previous distance`);
            lastDistanceSeen = doc.dist;
        }
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        let P = Math.floor(Math.sqrt(this.nDocs));

        // Move the point to another location within the PxP grid.
        let newX = Random.randInt(P) - P / 2;
        let newY = Random.randInt(P) - P / 2;
        return {$set: {geo: [newX, newY]}};
    };

    $config.data.getIndexSpec = function getIndexSpec() {
        return {geo: "2d"};
    };

    $config.data.getReplaceSpec = function getReplaceSpec(i, coords) {
        return {_id: i, geo: coords};
    };

    /*
     * Insert some docs in geo form and make a 2d index.
     */
    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        let P = Math.floor(Math.sqrt(this.nDocs));
        let i = 0;
        // Set up some points to query (in a PxP grid around 0,0).
        let bulk = db[collName].initializeUnorderedBulkOp();
        for (let x = 0; x < P; x++) {
            for (let y = 0; y < P; y++) {
                let coords = [x - P / 2, y - P / 2];
                bulk.find({_id: i}).upsert().replaceOne(this.getReplaceSpec(i, coords));
                i++;
            }
        }
        assert.commandWorked(bulk.execute());
        assert.commandWorked(db[collName].createIndex(this.getIndexSpec()));
    };

    return $config;
});
