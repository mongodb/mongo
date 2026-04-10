/**
 * Tests that collMod does not accidentally target a regular collection as timeseries.
 *
 * This is done by continuously swapping one collection between:
 * - A regular collection with a {meta: 1} index.
 * - A timeseries collection with a metaField "m" but no {m: 1} logical index.
 *   * Therefore this collection does not have a {meta: 1} raw/physical index.
 *
 * What we want to prevent is that a collMod over the {m: 1} index:
 * - Starts by considering the collection as timeseries, so it translates {m: 1} to {meta: 1}.
 * - Meanwhile the collection is re-created as a regular collection with a {meta: 1} index.
 * - Then, it succeeds targetting the {meta: 1} index on the regular collection.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Only viewless timeseries collections can be renamed over.
 *   featureFlagCreateViewlessTimeseriesCollections,
 * ]
 */

import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

export const $config = (function () {
    const runningWithStepdowns = TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns;

    let states = {
        collMod: function (db, collName) {
            assert.commandFailedWithCode(
                db.runCommand({collMod: collName, index: {keyPattern: {m: 1}, hidden: true}}),
                [
                    // As expected, we did not find the {m: 1}
                    ErrorCodes.IndexNotFound,
                    // collMod targeted the collection while it was dropped
                    ErrorCodes.NamespaceNotFound,
                ],
            );
        },

        createTimeseriesCollection: function (db, collName) {
            assert.commandWorkedOrFailedWithCode(
                db.createCollection(collName, {timeseries: {timeField: "t", metaField: "m"}}),
                [
                    ErrorCodes.NamespaceExists,
                    // In suites that implicitly track the timeseries collection, the router could exhaust
                    // the retry attempts on StaleConfig because we repeatedly drop and re-create the collection,
                    // causing its shard version to change continuously.
                    ErrorCodes.StaleConfig,
                ],
            );
        },

        createIndex: function (db, collName) {
            // Create a logical {meta: 1} index. This should not be targetted by collMod.
            // This will implicitly create a regular collection if it does not exist.
            assert.commandWorkedOrFailedWithCode(db[collName].createIndex({meta: 1}), ErrorCodes.NamespaceExists);
        },

        drop: function (db, collName) {
            assert(db[collName].drop());
        },

        renameCollectionIntoPlace: function (db, collName) {
            const tmpName = collName + "_tmpts_" + this.tid;
            assert.commandWorked(db[tmpName].createIndex({meta: 1}));
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({
                    renameCollection: db[tmpName].getFullName(),
                    to: db[collName].getFullName(),
                    dropTarget: true,
                }),
                // Rename is not idempotent, so a retry after stepdown may fail to find the source.
                runningWithStepdowns ? [ErrorCodes.NamespaceNotFound] : [],
            );
        },
    };

    return {
        threadCount: 4,
        iterations: 100,
        states: states,
        startState: "collMod",
        transitions: uniformDistTransitions(states),
    };
})();
