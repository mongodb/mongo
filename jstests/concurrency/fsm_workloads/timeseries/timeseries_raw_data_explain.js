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
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/timeseries/timeseries_raw_data_operations.js";
import {getPlanStage} from "jstests/libs/query/analyze_plan.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.assertExplain = function(db, commandResult, commandName) {
        const coll = this.getMainCollection(db);
        assert(commandResult.ok);
        if (commandResult.command.pipeline) {
            for (const stage of commandResult.command.pipeline) {
                assert.isnull(stage["$_internalUnpackBucket"],
                              `Expected not to find $_internalUnpackBucket stage`);
            }
        }
        assert.isnull(getPlanStage(commandResult, "TS_MODIFY")),
            "Expected not to find TS_MODIFY stage " + tojson(commandResult);
        assert(commandResult.command.rawData,
               `Expected command to include rawData but got ${tojson(commandResult)}`);
        assert.eq(commandResult.command[commandName],
                  coll.getName(),
                  `Expected command namespace to be ${tojson(coll.getName())} but got ${
                      tojson(commandResult.command[commandName])}`);
    };

    $config.states.explainAggregate = function explainAggregate(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db,
            coll.explain().aggregate([{$match: {[this.thisThreadKey]: this.tid}}], {rawData: true}),
            "aggregate");
    };

    $config.states.explainCount = function explainCount(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db, coll.explain().count({[this.thisThreadKey]: this.tid}, {rawData: true}), "count");
    };

    $config.states.explainDistinct = function explainDistinct(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db, coll.explain().distinct(this.thisThreadKey, {}, {rawData: true}), "distinct");
    };

    $config.states.explainFind = function explainFind(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db, coll.explain().find({[this.thisThreadKey]: this.tid}).rawData().finish(), "find");
    };

    $config.states.explainFindAndModify = function explainFindAndModify(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(db,
                           coll.explain().findAndModify({
                               query: {"control.count": 2},
                               update: {$set: this.getNextMetaField()},
                               rawData: true,
                           }),
                           "findAndModify");
    };

    $config.states.explainUpdate = function explainUpdate(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db,
            coll.explain().update({"control.count": 1}, {$set: {meta: "3"}}, {rawData: true}),
            "update");
    };

    $config.states.explainDelete = function explainDelete(db, collName) {
        const coll = this.getMainCollection(db);
        this.assertExplain(
            db, coll.explain().remove({"control.count": 1}, {rawData: true}), "delete");
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
