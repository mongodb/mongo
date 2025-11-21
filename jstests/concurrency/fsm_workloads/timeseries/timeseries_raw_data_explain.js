/**
 * Extends timeseries_raw_data_operations.js to include explain.
 *
 * @tags: [
 *  requires_timeseries,
 *  requires_fcv_82,
 *  # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *  does_not_support_stepdowns,
 *  requires_getmore,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/timeseries/timeseries_raw_data_operations.js";
import {
    areViewlessTimeseriesEnabled,
    getTimeseriesCollForDDLOps,
} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {isFCVlt} from "jstests/libs/feature_compatibility_version.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getPlanStage} from "jstests/libs/query/analyze_plan.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.assertExplain = function (db, commandResult, commandName) {
        const coll = this.getMainCollection(db);
        assert(commandResult.ok);
        if (commandResult.command.pipeline) {
            for (const stage of commandResult.command.pipeline) {
                assert.isnull(stage["$_internalUnpackBucket"], `Expected not to find $_internalUnpackBucket stage`);
            }
        }
        assert.isnull(getPlanStage(commandResult, "TS_MODIFY")),
            "Expected not to find TS_MODIFY stage " + tojson(commandResult);
        assert(commandResult.command.rawData, `Expected command to include rawData but got ${tojson(commandResult)}`);
        let expectedNss = (() => {
            if (
                commandResult.command.findAndModify &&
                FixtureHelpers.isTracked(getTimeseriesCollForDDLOps(db, coll)) &&
                !areViewlessTimeseriesEnabled(db)
            ) {
                // In sharded clusters for findAndModify over legacy tracked timeseries we convert the namespace on the router and we send the command
                // with translated namespace to the shard,
                // thus we expect explain to report the command targeting system.buckets internal namespace.
                return getTimeseriesCollForDDLOps(db, coll).getName();
            }
            return coll.getName();
        })();
        if (commandResult.command.findAndModify && isFCVlt(db.getMongo(), "8.3")) {
            // In versions 8.2 findAndModify explain return the main namespace instead of the system.buckets
            // TODO SERVER-114161 enable the check also for findAndModify once the fix have been backported to previous versions
            jsTest.log(
                "Skipping namespace check for findAndModify explain output since FCV is less then 8.3 (BACKPORT-26389)",
            );
        } else if (
            commandResult.command.findAndModify &&
            !areViewlessTimeseriesEnabled(db) &&
            TestData.runningWithBalancer &&
            FixtureHelpers.isTracked(getTimeseriesCollForDDLOps(db, coll)) &&
            !FixtureHelpers.isSharded(getTimeseriesCollForDDLOps(db, coll))
        ) {
            // If the collection is tracked or untracked findAndModify explain returns either the buckets or main timeseries namespace
            // In suites with enabled balancer the collection could randomly became tracked.
            jsTest.log(
                "Skipping namespace check for findAndModify explain output in suite with random balancer enabled since we don't know if the collection was tracked or not when the command was executed",
            );
        } else {
            assert.eq(
                commandResult.command[commandName],
                expectedNss,
                `Expected command namespace to be ${tojson(expectedNss)} but got ${tojson(commandResult.command[commandName])}. Full explain output: ${tojson(commandResult)}`,
            );
        }
    };

    $config.states.explainAggregate = function explainAggregate(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db,
            coll.explain().aggregate([{$match: {[this.thisThreadKey]: this.tid}}], {rawData: true}),
            "aggregate",
        );
    };

    $config.states.explainCount = function explainCount(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(db, coll.explain().count({[this.thisThreadKey]: this.tid}, {rawData: true}), "count");
    };

    $config.states.explainDistinct = function explainDistinct(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(db, coll.explain().distinct(this.thisThreadKey, {}, {rawData: true}), "distinct");
    };

    $config.states.explainFind = function explainFind(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db,
            coll
                .explain()
                .find({[this.thisThreadKey]: this.tid})
                .rawData()
                .finish(),
            "find",
        );
    };

    $config.states.explainFindAndModify = function explainFindAndModify(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db,
            coll.explain().findAndModify({
                query: {"control.count": 2},
                update: {$set: this.getNextMetaField()},
                rawData: true,
            }),
            "findAndModify",
        );
    };

    $config.states.explainUpdate = function explainUpdate(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db,
            coll.explain().update({"control.count": 1}, {$set: {meta: "3"}}, {rawData: true}),
            "update",
        );
    };

    $config.states.explainDelete = function explainDelete(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(db, coll.explain().remove({"control.count": 1}, {rawData: true}), "delete");
    };

    const standardTransition = {
        findInitial: 1,
        findAll: 1,
        addBucketByMeasurement: 1,
        insert: 1,
        deleteOne: 1,
        count: 1,
        aggregate: 1,
        distinct: 1,
        explainAggregate: 1,
        explainCount: 1,
        explainDistinct: 1,
        explainFind: 1,
        explainFindAndModify: 1,
        explainUpdate: 1,
        explainDelete: 1,
    };

    $config.transitions = {
        findInitial: standardTransition,
        findAll: standardTransition,
        addBucketByMeasurement: standardTransition,
        insert: standardTransition,
        deleteOne: standardTransition,
        count: standardTransition,
        aggregate: standardTransition,
        distinct: standardTransition,
        explainAggregate: standardTransition,
        explainCount: standardTransition,
        explainDistinct: standardTransition,
        explainFind: standardTransition,
        explainFindAndModify: standardTransition,
        explainUpdate: standardTransition,
        explainDelete: standardTransition,
    };

    return $config;
});
