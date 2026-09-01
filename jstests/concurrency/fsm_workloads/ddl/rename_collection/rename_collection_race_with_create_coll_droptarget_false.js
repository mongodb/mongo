/**
 * Similar to rename_collection_race_with_create_coll_droptarget_true.js but with
 * dropTarget = false. Rename command here can either fail with NamespaceExists (target got created)
 * by a parallel CreateCollection OR it can succeed. A successful rename moves the markers;
 * NamespaceExists leaves them on the source and does not update the recorded source collection.
 *
 * @tags: [
 *   requires_sharding,
 *   # The mutex may be left locked if a stepdown interrupts its findAndModify/update sequence.
 *   does_not_support_stepdowns,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/ddl/rename_collection/rename_collection_race_with_create_coll_droptarget_true.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.states.rename = function (db, collName) {
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

            const res = db.adminCommand({
                renameCollection: sourceColl.getFullName(),
                to: targetColl.getFullName(),
                dropTarget: false,
            });
            assert.commandWorkedOrFailedWithCode(res, ErrorCodes.NamespaceExists);

            if (res.ok) {
                this.assertMarkersPresent(targetColl);
                assert.eq(0, sourceColl.countDocuments({}));
                assert.commandWorked(
                    controlCollection.updateOne(
                        {_id: this.controlDocumentId},
                        {$set: {sourceCollName: targetColl.getName()}},
                    ),
                );
            } else {
                this.assertMarkersPresent(sourceColl);
            }
        } finally {
            this.mutexUnlock(collName);
        }
    };

    return $config;
});
