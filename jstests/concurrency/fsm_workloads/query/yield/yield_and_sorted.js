/*
 * yield_and_sorted.js (extends yield_rooted_or.js)
 *
 * Intersperse queries which use the AND_SORTED stage with updates and deletes of documents they
 * may match.
 * @tags: [
 *  # internalQueryForceIntersectionPlans knob could affect concurrent tests to use unexpected plan.
 *  incompatible_with_concurrency_simultaneous,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/query/yield/yield_rooted_or.js";
import {planHasStage} from "jstests/libs/query/analyze_plan.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    /*
     * Issue a query that will use the AND_SORTED stage.
     */
    $config.states.query = function andSorted(db, collName) {
        const query = {c: 0, d: this.nDocs};

        // Not very many docs returned in this, so loop to increase chances of yielding in the
        // middle.
        for (var i = 0; i < 100; i++) {
            const explain = db[collName].explain().find(query).finish();
            assert(planHasStage(db, explain.queryPlanner.winningPlan, 'AND_SORTED'));

            const res = db[collName].find(query).toArray();
            for (const result of res) {
                // These should always be true, since they're just verifying that the results
                // match
                // the query predicate.
                assert.eq(result.c, 0);
                assert.eq(result.d, this.nDocs);
            }
        }
    };

    $config.data.originalQueryPlannerEnableHashIntersection = {};
    $config.data.originalQueryPlannerEnableIndexIntersection = {};
    $config.data.originalQueryForceIntersectionPlans = {};

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        cluster.executeOnMongodNodes((db) => {
            const res1 = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryPlannerEnableHashIntersection: true,
            }));
            this.originalQueryPlannerEnableHashIntersection[db.getMongo().host] = res1.was;
            const res2 = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryPlannerEnableIndexIntersection: true,
            }));
            this.originalQueryPlannerEnableIndexIntersection[db.getMongo().host] = res2.was;
            const res3 = assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryForceIntersectionPlans: true,
            }));
            this.originalQueryForceIntersectionPlans[db.getMongo().host] = res3.was;
        });
    };

    $config.teardown = function teardown(db, collName, cluster) {
        $super.teardown.apply(this, arguments);

        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryPlannerEnableHashIntersection:
                    this.originalQueryPlannerEnableHashIntersection[db.getMongo().host],
            }));
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryPlannerEnableIndexIntersection:
                    this.originalQueryPlannerEnableIndexIntersection[db.getMongo().host],
            }));
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                internalQueryForceIntersectionPlans:
                    this.originalQueryForceIntersectionPlans[db.getMongo().host],
            }));
        });
    };

    return $config;
});
