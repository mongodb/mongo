'use strict';

/*
 * yield_and_sorted.js (extends yield_rooted_or.js)
 *
 * Intersperse queries which use the AND_SORTED stage with updates and deletes of documents they
 * may match.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');       // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield_rooted_or.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

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
                        endKeyInclusive: false,
                        direction: -1
                    }
                }
            };

            // Intersect the two
            var andix1ix2 = {andSorted: {args: {nodes: [ixscan1, ixscan2]}}};
            var res = db.runCommand({stageDebug: {collection: collName, plan: andix1ix2}});
            assertAlways.commandWorked(res);
            for (var j = 0; j < res.results.length; j++) {
                var result = res.results[j];
                // These should always be true, since they're just verifying that the results
                // match
                // the query predicate.
                assertAlways.eq(result.c, 0);
                assertAlways.eq(result.d, this.nDocs);
            }
        }
    };

    return $config;
});
