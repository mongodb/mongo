/**
 * Repeatedly run collMod on the collection options along with creating and dropping indexes of a
 * collection.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */

import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

export const $config = (function() {
    const states = {
        init: function(db, collName) {
            const data = [];
            for (let i = 0; i < 500; i++) {
                data.push({x: Random.randInt(100)});
            }
            db[collName].insertMany(data);
        },
        collMod: function(db, collName) {
            const collModOp =
                Random.randInt(2) == 0 ? {validator: {y: {$exists: false}}} : {validator: {}};
            assert.commandWorkedOrFailedWithCode(db.runCommand({collMod: collName, ...collModOp}), [
                // Conflicted with another thread's collMod command
                ErrorCodes.ConflictingOperationInProgress,
            ]);
        },
        createIndex: function(db, collName) {
            assert.commandWorkedOrFailedWithCode(db[collName].createIndex({x: 1}), [
                ErrorCodes.IndexBuildAlreadyInProgress,
                // The index was dropped while it was being built
                // TODO SERVER-75675: Remove once createIndex serializes with dropIndex
                ErrorCodes.IndexBuildAborted,
            ]);
        },
        dropIndex: function(db, collName) {
            assert.commandWorkedOrFailedWithCode(db[collName].dropIndex({x: 1}), [
                // TODO SERVER-107420: Remove IndexNotFound from acceptable dropIndexes errors
                // once 9.0 becomes LTS
                ErrorCodes.IndexNotFound,
            ]);
        },
        checkMetadataConsistency: function(db, collName) {
            const inconsistencies = db[collName].checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
    };

    return {
        threadCount: 5,
        iterations: 200,
        states: states,
        startState: "init",
        transitions: uniformDistTransitions(states),
    };
})();
