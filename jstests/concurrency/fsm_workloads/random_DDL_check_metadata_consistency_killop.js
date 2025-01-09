/**
 * Tests that killing the checkMetadataConsistency operation works as expected,
 * i.e. it is either interrupted, or that it completes if the interruption arrives too late.
 * DDL operations running in the background add diversity to the test set and timings.
 *
 * @tags: [
 *   requires_sharding,
 *   # killOp does not support stepdowns.
 *   does_not_support_stepdowns,
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_DDL_operations.js";

function withCatchAndIgnoreInterrupted(fn) {
    try {
        fn();
        print("checkMetadataConsistency operation completed successfully");
    } catch (e) {
        if (e.code === ErrorCodes.Interrupted) {
            print("checkMetadataConsistency operation was interrupted");
            return;
        }

        throw e;
    }
}

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Mark our commands with a comment, to avoid killing operations from test hooks
    const kMarkerComment = "check_metadata_consistency_killop_marker";

    $config.states.killCheckMetadataConsistency = function(db, collName, connCache) {
        const ops = db.getSiblingDB('admin')
                        .aggregate([
                            {$currentOp: {localOps: true}},
                            {
                                $match: {
                                    'command.checkMetadataConsistency': {$exists: true},
                                    'command.comment': kMarkerComment
                                }
                            },
                            {$project: {opid: '$opid'}}
                        ])
                        .toArray();

        for (const op of ops) {
            print('Killing checkMetadataConsistency with opid: ' + tojson(op.opid));
            assert.commandWorked(db.getSiblingDB('admin').killOp(op.opid));
        }
    };

    // Override the base states to tolerate the interrupted error and add the marker comment
    $config.states.checkDatabaseMetadataConsistency = function(db, collName, connCache) {
        db = this.getRandomDb(db);
        jsTestLog('Executing checkMetadataConsistency state for database: ' + db.getName());
        withCatchAndIgnoreInterrupted(() => {
            const inconsistencies = db.getSiblingDB("admin")
                                        .checkMetadataConsistency({comment: kMarkerComment})
                                        .toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        });
    };

    $config.states.checkCollectionMetadataConsistency = function(db, collName, connCache) {
        db = this.getRandomDb(db);
        const coll = this.getRandomCollection(db);
        jsTestLog('Executing checkMetadataConsistency state for collection: ' + coll.getFullName());
        withCatchAndIgnoreInterrupted(() => {
            const inconsistencies =
                coll.checkMetadataConsistency({comment: kMarkerComment}).toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        });
    };

    // Add a state for the cluster-wide checkMetadataConsistency check for better coverage
    $config.states.checkClusterMetadataConsistency = function(db, collName, connCache) {
        jsTestLog('Executing checkMetadataConsistency state for cluster');
        withCatchAndIgnoreInterrupted(() => {
            const inconsistencies = db.getSiblingDB("admin")
                                        .checkMetadataConsistency({comment: kMarkerComment})
                                        .toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        });
    };

    $config.transitions = uniformDistTransitions($config.states);

    return $config;
});
