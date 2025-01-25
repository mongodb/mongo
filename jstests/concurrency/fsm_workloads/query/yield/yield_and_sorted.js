/*
 * yield_and_sorted.js (extends yield_rooted_or.js)
 *
 * Intersperse queries which use the AND_SORTED stage with updates and deletes of documents they
 * may match.
 *
 * @tags: [
 *   # stagedebug can only be run against a standalone mongod
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/yield/yield_rooted_or.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    /*
     * Issue a query that will use the AND_SORTED stage. This is a little tricky, so use
     * stagedebug to force it to happen. Unfortunately this means it can't be batched.
     */
    $config.states.query = function andSorted(db, collName) {
        // Not very many docs returned in this, so loop to increase chances of yielding in the
        // middle.
        for (var i = 0; i < 100; i++) {
            // Construct the query plan: two ixscans under an andSorted.
            // Scan a == 0
            var ixscan1 = {
                ixscan: {
                    args: {
                        name: 'stages_and_sorted',
                        keyPattern: {c: 1},
                        startKey: {'': 0},
                        endKey: {'': 0},
                        startKeyInclusive: true,
                        endKeyInclusive: false,
                        direction: 1
                    }
                }
            };
            // Scan b == this.nDocs
            var ixscan2 = {
                ixscan: {
                    args: {
                        name: 'stages_and_sorted',
                        keyPattern: {d: 1},
                        startKey: {'': this.nDocs},
                        endKey: {'': this.nDocs},
                        startKeyInclusive: true,
                        endKeyInclusive: false,
                        direction: -1
                    }
                }
            };

            // Intersect the two
            var andix1ix2 = {andSorted: {args: {nodes: [ixscan1, ixscan2]}}};
            var res = db.runCommand({stageDebug: {collection: collName, plan: andix1ix2}});
            assert.commandWorked(res);
            for (var j = 0; j < res.results.length; j++) {
                var result = res.results[j];
                // These should always be true, since they're just verifying that the results
                // match
                // the query predicate.
                assert.eq(result.c, 0);
                assert.eq(result.d, this.nDocs);
            }
        }
    };

    return $config;
});
