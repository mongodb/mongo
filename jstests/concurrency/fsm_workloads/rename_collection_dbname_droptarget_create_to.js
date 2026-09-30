/**
 * rename_collection_dbname_droptarget_create_to.js
 *
 * Creates a collection and then repeatedly executes the renameCollection
 * command against it, specifying a different database name in the namespace.
 *
 * This test verifies that rename supports the "to" collection being created during the operation.
 * While each thread renames its own "from" and "to" with dropTarget=true (i.e. renames do not
 * interact with each other), threads concurrently create the "to" being used by other threads.
 *
 * @tags: [
 *     # Rename between DBs with different shard primary is not supported
 *     assumes_unsharded_collection,
 * ]
 */

import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

export const $config = (function () {
    const prefix = jsTestName();

    function fromDBName(tid) {
        return prefix + tid + "_fromdb";
    }

    function toDBName(tid) {
        return prefix + tid + "_todb";
    }

    const states = {
        init: function (db, collName) {},

        rename: function (db, collName) {
            // Use different DBs to prevent renames interaction across threads
            const fromDB = db.getSiblingDB(fromDBName(this.tid));
            const toDB = db.getSiblingDB(toDBName(this.tid));

            // Create the "from" collection with a document
            assert.commandWorked(fromDB[collName].insertOne({x: 1}));

            // Verify that a document exist in the "to" collection after the rename occurs
            assert.commandWorked(
                fromDB.adminCommand({
                    renameCollection: fromDB.getName() + "." + collName,
                    to: toDB.getName() + "." + collName,
                    dropTarget: true,
                }),
            );

            assert.eq(1, toDB[collName].find().itcount());
            assert.eq(0, fromDB[collName].find().itcount());

            // Drop the "to" collection so `createTo` can recreate it
            assert(toDB[collName].drop());
        },

        createTo: function (db, collName) {
            // Create the "to" collection of _another_ thread's rename
            // This won't change the outcome of a concurrent rename,
            // but it will test that the rename drops it even if it appears mid-operation
            const otherTid = Random.randInt(this.threadCount);
            const toDB = db.getSiblingDB(toDBName(otherTid));
            assert.commandWorkedOrFailedWithCode(toDB.createCollection(collName), ErrorCodes.NamespaceExists);
        },
    };

    return {
        threadCount: 4,
        iterations: 10,
        states: states,
        transitions: uniformDistTransitions(states),
    };
})();
