/**
 * Test all kinds of collection creation concurrently.
 *
 * @tags: [
 *  requires_sharding,
 *  # TODO SERVER-84659: remove the following tag once the old create collection
 *  # coordinator is fixed
 *  featureFlagAuthoritativeShardCollection,
 *  # this test swallow expected errors caused by concurrent DDL operations
 *  catches_command_failures
 * ]
 */

import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

const testDbName = jsTestName() + "_DB";
const collPrefix = "coll_";
const collCount = 12;

export const $config = (function() {
    function getRandomCollection(db) {
        return db.getSiblingDB(testDbName)[collPrefix + Random.randInt(collCount)];
    }

    const states = {
        init: function init(db, collName) {},

        createUnsharded: function createUnsharded(db, collName) {
            const coll = getRandomCollection(db);
            jsTestLog("Executing state createUnsharded: " + coll.getFullName());

            const res = coll.getDB().createCollection(coll.getName());
            const errorCodes = [
                // Concurrent creation on the same namespace.
                ErrorCodes.NamespaceExists
            ];
            assert.commandWorkedOrFailedWithCode(
                res, errorCodes, "Failed to create unsharded collection");
        },

        createView: function createView(db, collName) {
            if (TestData.runInsideTransaction) {
                // TODO SERVER-50484: creating a timeseries/view is not supported
                // in multi-document transactions.
                return;
            }
            const baseColl = getRandomCollection(db);
            const viewColl = getRandomCollection(db);
            jsTestLog("Executing state createView: viewNss: " + viewColl.getFullName() +
                      " baseColl: " + baseColl.getFullName());

            const res = baseColl.getDB().createCollection(
                viewColl.getName(), {viewOn: baseColl.getName(), pipeline: [{"$match": {}}]});
            const errorCodes = [
                // Concurrent creation on the same namespace.
                ErrorCodes.NamespaceExists,
                // When creating a view with random names, there is the possibility of creating a
                // dependency graph.
                ErrorCodes.GraphContainsCycle
            ];
            assert.commandWorkedOrFailedWithCode(res, errorCodes, "Failed to create view");
        },

        createTimeseriesUnsharded: function createTimeseriesUnsharded(db, collName) {
            if (TestData.runInsideTransaction) {
                // TODO SERVER-50484: creating a timeseries/view is not supported
                // in multi-document transactions.
                return;
            }
            const coll = getRandomCollection(db);
            jsTestLog("Executing state createTimeseriesUnsharded: " + coll.getFullName());

            const res =
                coll.getDB().createCollection(coll.getName(), {timeseries: {timeField: "time"}});
            const errorCodes = [
                // Concurrent creation on the same namespace.
                ErrorCodes.NamespaceExists
            ];
            assert.commandWorkedOrFailedWithCode(
                res, errorCodes, "Failed to create unsharded timeseries");
        },

        createTimeseriesSharded: function createTimeseriesSharded(db, collName) {
            if (TestData.runInsideTransaction) {
                // TODO SERVER-50484: creating a timeseries/view is not supported
                // in multi-document transactions.
                return;
            }

            const coll = getRandomCollection(db);
            jsTestLog("Executing state createTimeseriesSharded: " + coll.getFullName());

            const res = db.adminCommand({
                shardCollection: coll.getFullName(),
                key: {time: 1},
                unique: false,
                timseries: {timeField: "time"}
            });
            const errorCodes = [
                // Concurrent creation on the same namespace.
                ErrorCodes.NamespaceExists,
                // Concurrent shard collection with different parameters.
                ErrorCodes.ConflictingOperationInProgress,
                // If the collection is not empty, the index must exist prior to command.
                ErrorCodes.InvalidOptions,
                // Trying to shard directly a view.
                ErrorCodes.CommandNotSupportedOnView
            ];
            assert.commandWorkedOrFailedWithCode(
                res, errorCodes, "Failed to create sharded timeseries");
        },

        createSharded: function createSharded(db, collName) {
            const coll = getRandomCollection(db);
            jsTestLog("Executing state createSharded: " + coll.getFullName());

            const res = db.adminCommand(
                {shardCollection: coll.getFullName(), key: {time: 1}, unique: false});
            const errorCodes = [
                // Concurrent creation on the same namespace.
                ErrorCodes.NamespaceExists,
                // Concurrent shard collection with different parameters.
                ErrorCodes.ConflictingOperationInProgress,
                // If the collection is not empty, the index must exist prior to command.
                ErrorCodes.InvalidOptions,
                // Trying to shard directly a view.
                ErrorCodes.CommandNotSupportedOnView
            ];
            assert.commandWorkedOrFailedWithCode(
                res, errorCodes, "Failed to create sharded collection");
        },

        createImplicitCollectionOrInsert: function insert(db, collName) {
            const coll = getRandomCollection(db);
            jsTestLog("Executing state createImplicitCollection or insert: " + coll.getFullName());

            const res = db.runCommand({insert: coll.getName(), documents: [{time: ISODate()}]});
            const errorCodes = [
                // It is not possible to insert into a view.
                ErrorCodes.CommandNotSupportedOnView,
            ];
            assert.commandWorkedOrFailedWithCode(
                res, errorCodes, "Failed to createImplicitCollection or insert");
        },

        drop: function drop(db, collName) {
            const coll = getRandomCollection(db);
            jsTestLog("Executing state drop: " + coll.getFullName());

            assert(coll.drop());
        },
    };

    return {
        threadCount: 12,
        iterations: 64,
        states: states,
        transitions: uniformDistTransitions(states),
    };
})();
