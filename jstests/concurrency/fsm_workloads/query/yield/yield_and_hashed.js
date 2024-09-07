/*
 * yield_and_hashed.js (extends yield_rooted_or.js)
 *
 * Intersperse queries which use the AND_HASH stage with updates and deletes of documents they may
 * match.
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
     * Issue a query that will use the AND_HASH stage. This is a little tricky, so use
     * stagedebug to force it to happen. Unfortunately this means it can't be batched.
     */
    $config.states.query = function andHash(db, collName) {
        var nMatches = 100;
        assert.lte(nMatches, this.nDocs);
        // Construct the query plan: two ixscans under an andHashed.
        // Scan c <= nMatches
        var ixscan1 = {
            ixscan: {
                args: {
                    name: 'stages_and_hashed',
                    keyPattern: {c: 1},
                    startKey: {'': nMatches},
                    endKey: {},
                    startKeyInclusive: true,
                    endKeyInclusive: true,
                    direction: -1
                }
            }
        };

        // Scan d >= this.nDocs - nMatches
        var ixscan2 = {
            ixscan: {
                args: {
                    name: 'stages_and_hashed',
                    keyPattern: {d: 1},
                    startKey: {'': this.nDocs - nMatches},
                    endKey: {},
                    startKeyInclusive: true,
                    endKeyInclusive: true,
                    direction: 1
                }
            }
        };

        var andix1ix2 = {andHash: {args: {nodes: [ixscan1, ixscan2]}}};

        // On non-MMAP storage engines, index intersection plans will always re-filter
        // the docs to make sure we don't get any spurious matches.
        var fetch = {
            fetch: {
                filter: {c: {$lte: nMatches}, d: {$gte: (this.nDocs - nMatches)}},
                args: {node: andix1ix2}
            }
        };

        var res = db.runCommand({stageDebug: {plan: fetch, collection: collName}});
        assert.commandWorked(res);
        for (var i = 0; i < res.results.length; i++) {
            var result = res.results[i];
            assert.lte(result.c, nMatches);
            assert.gte(result.d, this.nDocs - nMatches);
        }
    };

    return $config;
});
