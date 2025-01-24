/*
 * yield_and_hashed.js (extends yield_rooted_or.js)
 *
 * Intersperse queries which use the AND_HASH stage with updates and deletes of documents they may
 * match.
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
     * Issue a query that will use the AND_HASH stage.
     */
    $config.states.query = function andHash(db, collName) {
        const nMatches = 100;
        assert.lte(nMatches, this.nDocs);
        const query = {c: {$lte: nMatches}, d: {$gte: this.nDocs - nMatches}};

        const explain = db[collName].explain().find(query).finish();
        assert(planHasStage(db, explain.queryPlanner.winningPlan, 'AND_HASH'));

        const res = db[collName].find(query).toArray();
        for (const result of res) {
            assert.lte(result.c, nMatches);
            assert.gte(result.d, this.nDocs - nMatches);
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
