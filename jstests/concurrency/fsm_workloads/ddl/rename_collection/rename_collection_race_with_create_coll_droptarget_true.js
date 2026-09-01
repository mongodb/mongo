/**
 * Repeatedly renames a collection between three namespaces with dropTarget=true while other FSM
 * threads concurrently create collections in those namespaces. The idea is to ensure that the data
 * in the source collection during a rename stays consistent and does not get lost when a
 * concurrent CreateCollection operation in the target namespace is performed.
 *
 * Rename operations are serialized by a findAndModify-based mutex. The create state deliberately
 * does not acquire that mutex so it can recreate the target after it is dropped and before the
 * rename reaches the target.
 *
 * NOTE: This workload intentionally keeps the three test collections unsharded. The same
 * state-machine shape can be adapted later to cover sharded collections.
 *
 * @tags: [
 *   requires_sharding,
 *   # The mutex may be left locked if a stepdown interrupts its findAndModify/update sequence.
 *   does_not_support_stepdowns,
 * ]
 */

import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

export const $config = (function () {
    const data = {
        // Document used to ensure serialization of different RenameCollection operations.
        controlDocumentId: "rename_mutex",
        // Marker documents which the test tracks before and after rename.
        markerDocs: [{_id: "marker0"}, {_id: "marker1"}],
        // Suffix for the name of collection used to serialize different RenameCollection
        // operations.
        controlCollectionSuffix: "_control",

        getDataCollections: function (db, collName) {
            return [db[collName + "_A"], db[collName + "_B"], db[collName + "_C"]];
        },

        getControlCollection: function (db, collName) {
            return db[collName + this.controlCollectionSuffix];
        },

        /**
         * Used for mutual exclusion. Uses a collection to ensure atomicity on the read and update
         * operation.
         */
        mutexLock: function (collName) {
            jsTest.log.info("Trying to acquire mutexLock");
            let acquiredDoc;
            assert.soon(() => {
                const doc = this.mutexSessionDb[
                    collName + this.controlCollectionSuffix
                ].findAndModify({
                    query: {_id: this.controlDocumentId},
                    update: {$set: {mutex: 1}},
                });
                if (doc.mutex === 0) {
                    acquiredDoc = doc;
                    return true;
                }
                return false;
            });
            jsTest.log.info("Acquired mutexLock");
            return acquiredDoc;
        },

        mutexUnlock: function (collName) {
            assert.commandWorked(
                this.mutexSessionDb[collName + this.controlCollectionSuffix].updateOne(
                    {_id: this.controlDocumentId},
                    {$set: {mutex: 0}},
                ),
            );
            jsTest.log.info("Unlocked mutex");
        },

        assertMarkersPresent: function (collection) {
            assert.eq(this.markerDocs.length, collection.countDocuments({}));
            this.markerDocs.forEach((marker) => {
                assert.eq(marker, collection.findOne({_id: marker._id}));
            });
        },
    };

    const states = {
        init: function (db, collName) {
            // Use a separate session for mutex changes so that it is not wrapped inside FSM txn.
            this.mutexSession = db.getMongo().startSession();
            this.mutexSessionDb = this.mutexSession.getDatabase(db.getName());
        },

        rename: function (db, collName) {
            const dataCollections = this.getDataCollections(db, collName);
            const controlCollection = this.getControlCollection(db, collName);

            const controlDocument = this.mutexLock(collName);
            try {
                const sourceColl = db[controlDocument.sourceCollName];
                assert(sourceColl);

                const possibleTargets = dataCollections.filter(
                    (collection) => collection.getName() !== sourceColl.getName(),
                );
                const targetColl = possibleTargets[Random.randInt(possibleTargets.length)];

                // Dropping the target explicitly creates the race window. A create state running
                // on another FSM thread may recreate this namespace before renameCollection runs.
                targetColl.drop();

                assert.commandWorked(
                    db.adminCommand({
                        renameCollection: sourceColl.getFullName(),
                        to: targetColl.getFullName(),
                        dropTarget: true,
                    }),
                );

                this.assertMarkersPresent(targetColl);
                assert.eq(0, sourceColl.countDocuments({}));
                assert.commandWorked(
                    controlCollection.updateOne(
                        {_id: this.controlDocumentId},
                        {$set: {sourceCollName: targetColl.getName()}},
                    ),
                );
            } finally {
                this.mutexUnlock(collName);
            }
        },

        create: function (db, collName) {
            const dataCollections = this.getDataCollections(db, collName);
            const collection = dataCollections[Random.randInt(dataCollections.length)];

            const res = db.createCollection(collection.getName());
            assert.commandWorkedOrFailedWithCode(res, [
                ErrorCodes.NamespaceExists,
                ErrorCodes.InvalidOptions,
            ]);
            if (!res.ok) {
                // This is useful when the state is run inside a txn.
                // Without this, if CreateCollection fails with NamespaceExists the FSM framework
                // thinks that the test passed and attempts to commits the transaction. However,
                // SERVER would have internally aborted the txn because of the NamespaceExists
                // error so you can't commit. As a result, when FSM commits, it gets a Transient
                // Error and retries the txn and hits the same issue again. This leads to indefinite
                // retry loop. With this, it will force FSM to retry the state outside of a txn and
                // thus next time it will pass.
                fsm.forceRunningOutsideTransaction(this);
            }
        },
    };

    const setup = function (db, collName, cluster) {
        const dataCollections = this.getDataCollections(db, collName);
        const controlCollection = this.getControlCollection(db, collName);

        dataCollections.forEach((collection) => collection.drop());
        controlCollection.drop();

        const initialCollection = dataCollections[0];
        assert.commandWorked(db.createCollection(initialCollection.getName()));
        assert.commandWorked(initialCollection.insertMany(this.markerDocs));

        assert.commandWorked(db.createCollection(controlCollection.getName()));
        assert.commandWorked(
            controlCollection.insertOne({
                _id: this.controlDocumentId,
                mutex: 0,
                sourceCollName: initialCollection.getName(),
            }),
        );
    };

    const teardown = function (db, collName, cluster) {
        const dataCollections = this.getDataCollections(db, collName);
        const controlCollection = this.getControlCollection(db, collName);
        dataCollections.forEach((collection) => collection.drop());
        controlCollection.drop();
    };

    return {
        threadCount: 10,
        iterations: 40,
        startState: "init",
        data,
        setup,
        teardown,
        states,
        transitions: uniformDistTransitions(states),
    };
})();
