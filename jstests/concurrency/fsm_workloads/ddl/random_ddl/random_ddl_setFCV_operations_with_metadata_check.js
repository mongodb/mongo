/**
 * Concurrently performs DDL commands and FCV changes and verifies metadata correctness after a
 * downgrade.
 *
 * @tags: [
 *   requires_sharding,
 *   # This test focus on metadata correctness in the presence of setFCV and DDL, and in order to
 *   # succesfully run setFCV we require having binaries in the same version, having binaries with
 *   # multiple versions would only add setFCV failures and would not test the desired behavior.
 *   multiversion_incompatible,
 *   # TODO (SERVER-88539) Remove the 'assumes_balancer_off' tag
 *   assumes_balancer_off,
 *   # TODO (SERVER-91702): Enable this test
 *   exclude_when_record_ids_replicated,
 *   # Interrupting resharding on stepdown suites might leave garbage that will make setFCV to fail
 *   does_not_support_stepdowns,
 *   # Currently all DDL are not compatible with transactions, it does not make sense to run this
 *   # test in transaction suites.
 *   does_not_support_transactions,
 *   # TODO (SERVER-104789): config shards cause setFCV to hang because resharding is not aborted.
 *   config_shard_incompatible
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";
import {
    $config as $baseConfig
} from "jstests/concurrency/fsm_workloads/ddl/random_ddl/random_ddl_setFCV_operations.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Counts the number of time the setFeatureCompatibility command succeeds.
    $config.data.succesfullSetFCVDbName = 'fcv';
    $config.data.succesfullSetFCVCollName = 'executions';
    $config.data.expectedSetFCVExecutions = 2;
    // Auxiliary helper to move a collection and capture all expected errors of the test.
    $config.data.kAcceptedMoveCollectionErrors = [
        // Only one resharding operation is allowed to be run at a point in time.
        ErrorCodes.ReshardCollectionInProgress,
        ErrorCodes.ConflictingOperationInProgress,
        // A concurrent drop operation could drop the collection that is about to be resharded.
        ErrorCodes.NamespaceNotFound,
        // Depending on the timing, a concurrent FCV could pass the first check of resharding, but
        // trigger another that uses the latest resharding options enabled only in 8.0.
        ErrorCodes.InvalidOptions,
        90675,
        // A concurrent FCV could abort the resharding operation.
        ErrorCodes.Interrupted,
        // In older versions there is no moveCollection.
        ErrorCodes.CommandNotSupported,
        // On FCV a moveCollection could be aborted.
        ErrorCodes.ReshardCollectionAborted,
    ];
    // Failures that are accepted for the createCollection command
    $config.data.kAcceptedCreateErrors = [
        // Ignore concurrent create with the same namespace.
        ErrorCodes.NamespaceExists,
        // In some suites with transactions, the internal commit transaction might be aborted.
        // Simply ignore the error and retry eventually.
        ErrorCodes.Interrupted,
        // If the create could not succeed due to a move primary, retry later.
        ErrorCodes.MovePrimaryInProgress,
        // Create could starve after a succesion of moveCollection/movePrimary.
        ErrorCodes.LockBusy,
        // A concurrent movePrimary might release the lock before finishing the refresh which would
        // make the creation to fail.
        ErrorCodes.StaleDbVersion,
    ];
    // Failures that are accepted for the setFeatureCompatibilityVersion command
    $config.data.kAcceptedSetFCVErrors = [
        // If setFCV failed due to collections in non primary shard (regardless of the best effort),
        // allow the state to run again eventually.
        ErrorCodes.CannotDowngrade,
        // Invalid fcv transition (e.g lastContinuous -> lastLTS).
        5147403,
        // Cannot upgrade FCV if a previous FCV downgrade stopped in the middle of cleaning up
        // internal server metadata.
        7428200,
        // Cannot downgrade FCV that requires a collMod command when index builds are concurrently
        // taking place.
        12587,
    ];

    // You might end up hitting a shard where the db might've moved.
    $config.data.kCheckMetadataConsistencyAllowedErrorCodes = [ErrorCodes.NamespaceNotFound];
    // In this FSM test the movePrimary command can end up starving due to resharding
    // taking time while holding the DDL lock.
    $config.data.kMovePrimaryAllowedErrorCodes.push(ErrorCodes.LockBusy);
    // Additionally, you might end up hitting a shard where the db have already moved.
    $config.data.kMovePrimaryAllowedErrorCodes.push(ErrorCodes.NamespaceNotFound);
    // TODO SERVER-105556: shardNotFound errors will be permitted in the base fsm eventually.
    $config.data.kMovePrimaryAllowedErrorCodes.push(ErrorCodes.ShardNotFound);

    // Auxiliary function to catch and handle the move collection errors.
    $config.data.moveCollectionHelper = function(db, nss, destinationShard) {
        try {
            assert.commandWorked(db.adminCommand({moveCollection: nss, toShard: destinationShard}));
        } catch (e) {
            if (!$config.data.kAcceptedMoveCollectionErrors.includes(e.code)) {
                throw e;
            }
        }
    };

    // Override create state to create unsharded collections instead of sharded collections.
    $config.states.create = function(db, collName, connCache) {
        db = $config.data.getRandomDb(db);
        const coll = $config.data.getRandomCollection(db);
        const fullNs = coll.getFullName();

        jsTestLog('STATE:create collection: ' + fullNs);
        try {
            assert.commandWorked(db.createCollection(coll.getName()));
        } catch (e) {
            if (!$config.data.kAcceptedCreateErrors.includes(e.code)) {
                throw e;
            }
        }
        jsTestLog('STATE:create finished succesfully');
    };

    // Move a random collection to a random shard, effectively tracking it.
    $config.states.moveCollection = function(db, collName, connCache) {
        db = $config.data.getRandomDb(db);
        const coll = $config.data.getRandomCollection(db);
        const fullNs = coll.getFullName();
        // The config server might stepdown while getting the shards.
        try {
            let shards = db.getSiblingDB('config').shards.find().toArray();
            const shardId = shards[Random.randInt(shards.length)]._id;

            jsTestLog('STATE:moveCollection  collection ' + fullNs + ' to shardId: ' + shardId);
            $config.data.moveCollectionHelper(db, fullNs, shardId);
        } catch (e) {
            if (!$config.data.kAcceptedMoveCollectionErrors.includes(e.code)) {
                throw e;
            }
        }
        jsTestLog('STATE:moveCollection finished succesfully');
    };

    $config.states.setFCV = function(db, collName, connCache) {
        try {
            const fcvValues = [latestFCV, lastContinuousFCV, lastLTSFCV];
            const targetFCV = fcvValues[Random.randInt(3)];
            // Ensure we're in the latest version.
            jsTestLog('STATE:setFCV setting FCV to ' + targetFCV);
            assert.commandWorked(
                db.adminCommand({setFeatureCompatibilityVersion: targetFCV, confirm: true}));
            db.getSiblingDB(
                  $config.data.succesfullSetFCVDbName)[$config.data.succesfullSetFCVCollName]
                .update({}, {$inc: {count: 1}});
        } catch (e) {
            if (!$config.data.kAcceptedSetFCVErrors.includes(e.code)) {
                throw e;
            }
        }
        jsTestLog('STATE:setFCV finished succesfully');
    };

    // This jstests adds new errors that can occur due to concurrent execution with resharding.
    // Unfortunately, the way extending works on fsm, we can't modify the error list for the
    // parent states, so, we need to also reimplement the states.
    $config.states.movePrimary = function(db, collName, connCache) {
        db = $config.data.getRandomDb(db);
        const shardId = $config.data.getRandomShard(connCache);

        jsTestLog('STATE:movePrimary moving: ' + db.getName() + ' to ' + shardId);
        const res = db.adminCommand({movePrimary: db.getName(), to: shardId});
        assert.commandWorkedOrFailedWithCode(res, $config.data.kMovePrimaryAllowedErrorCodes);
    };

    $config.states.checkDatabaseMetadataConsistency = function(db, collName, connCache) {
        db = $config.data.getRandomDb(db);
        jsTestLog('STATE:checkDatabaseMetadataConsistency for database: ' + db.getName());
        try {
            const inconsistencies = db.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        } catch (e) {
            if (!$config.data.kCheckMetadataConsistencyAllowedErrorCodes.includes(e.code)) {
                throw e;
            }
        }
    };

    $config.states.checkCollectionMetadataConsistency = function(db, collName, connCache) {
        db = $config.data.getRandomDb(db);
        const coll = $config.data.getRandomCollection(db);
        jsTestLog('STATE:checkCollectionMetadataConsistency for collection: ' + coll.getFullName());
        try {
            const inconsistencies = coll.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        } catch (e) {
            if (!$config.data.kCheckMetadataConsistencyAllowedErrorCodes.includes(e.code)) {
                throw e;
            }
        }
    };

    $config.transitions = uniformDistTransitions($config.states);

    $config.setup = function(db, collName, cluster) {
        db.getSiblingDB($config.data.succesfullSetFCVDbName)[$config.data.succesfullSetFCVCollName]
            .insert({count: 0});
    };

    $config.teardown = function(db, collName, cluster) {
        assert.commandWorked(
            db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));
        const fcvExecutions =
            db.getSiblingDB(
                  $config.data.succesfullSetFCVDbName)[$config.data.succesfullSetFCVCollName]
                .findOne({})
                .count;
        assert(fcvExecutions >= $config.data.expectedSetFCVExecutions,
               'Expected setFeatureCompatibility to run at least 2 times, but it ran ' +
                   fcvExecutions + ' times');
    };

    // Do more iterations than usual to ensure setFeatureCompatibility is called enough times to
    // have a significant test execution.
    $config.iterations = 128;
    return $config;
});
