/**
 * Performs convertToCapped concurrently to setFCV.
 *
 * @tags: [
 *   requires_capped,
 *   # Can't convert a sharded collection to a capped collection
 *   assumes_unsharded_collection,
 *   # Requires all nodes to be running the latest binary.
 *   multiversion_incompatible,
 *   # Runs setFCV, which can interfere with other tests.
 *   incompatible_with_concurrency_simultaneous,
 *   runs_set_fcv,
 *   # clusterTime (used for snapshot reads) is only returned if replication is enabled.
 *   requires_replication,
 *   # Specifying a timestamp for readConcern snapshot in a causally consistent session is not allowed.
 *   does_not_support_causal_consistency,
 * ]
 */
import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {handleRandomSetFCVErrors} from "jstests/concurrency/fsm_workload_helpers/fcv/handle_setFCV_errors.js";

export const $config = (function () {
    const states = {
        init: function (db, collName) {},

        create: function (db, collName) {
            assert.commandWorkedOrFailedWithCode(db.createCollection(collName), [
                // The collection may have already been converted to capped
                ErrorCodes.NamespaceExists,
            ]);
        },

        drop: function (db, collName) {
            db[collName].drop();
        },

        convertToCapped: function (db, collName) {
            assert.commandWorkedOrFailedWithCode(db.runCommand({convertToCapped: collName, size: 1024}), [
                ErrorCodes.NamespaceNotFound,
            ]);
        },

        setFCV: function (db, collName) {
            const fcvValues = [lastLTSFCV, latestFCV];
            const targetFCV = fcvValues[Random.randInt(2)];
            try {
                assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
            } catch (e) {
                if (handleRandomSetFCVErrors(e, targetFCV)) {
                    return;
                }
                throw e;
            }
        },

        // TODO(SERVER-78068): Remove once recordIdsReplicated downgrade code is removed.
        checkRecordIdsReplicatedConsistentWithFCV: function (db, collName) {
            try {
                // Fetch the FCV document and the catalog at a consistent point in time.
                const fcvRes = assert.commandWorked(
                    db
                        .getSiblingDB("admin")
                        .runCommand({find: "system.version", filter: {_id: "featureCompatibilityVersion"}}),
                );
                const fcvDoc = fcvRes.cursor.firstBatch[0];
                print("Got FCV document: " + tojsononeline(fcvRes));
                if (fcvDoc.version != "8.0" || fcvDoc.targetVersion) {
                    // Not on fully downgraded FCV 8.0
                    return;
                }

                // If the FCV is fully downgraded, recordIdsReplicated field should not be present.
                const atClusterTime = fcvRes.operationTime;
                const catalog = db[collName]
                    .aggregate([{$listCatalog: {}}], {readConcern: {level: "snapshot", atClusterTime}})
                    .toArray();
                for (const catalogEntry of catalog) {
                    assert(
                        !catalogEntry.md.options.recordIdsReplicated,
                        `Found recordIdsReplicated on FCV 8.0: ${tojson(catalog)}, FCV: ${tojson(fcvDoc)}`,
                    );
                }
            } catch (e) {
                // TODO(SERVER-119156): Remove once this issue is fixed.
                if (e.code == 10065) {
                    return;
                }

                if (e.code == ErrorCodes.SnapshotTooOld) {
                    return;
                }

                throw e;
            }
        },
    };

    const teardown = function (db, collName, cluster) {
        assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
    };

    return {
        threadCount: 4,
        iterations: 50,
        states,
        transitions: uniformDistTransitions(states),
        teardown,
    };
})();
