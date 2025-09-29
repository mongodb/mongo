/**
 * Concurrently performs DDL commands and verifies guarantees are not broken.
 *
 * @tags: [
 *   requires_sharding,
 *  ]
 */

import {uniformDistTransitions} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

export const $config = (function () {
    let data = {
        dbPrefix: jsTestName() + "_DB_",
        dbCount: 2,
        collPrefix: "sharded_coll_",
        collCount: 2,
        getRandomDb: function (db) {
            return db.getSiblingDB(this.dbPrefix + Random.randInt(this.dbCount));
        },
        getRandomCollection: function (db) {
            return db[this.collPrefix + Random.randInt(this.collCount)];
        },
        getRandomShard: function (connCache) {
            const shards = Object.keys(connCache.shards);
            return shards[Random.randInt(shards.length)];
        },
        kMovePrimaryAllowedErrorCodes: [
            ErrorCodes.ConflictingOperationInProgress,
            // The cloning phase has failed (e.g. as a result of a stepdown). When a failure
            // occurs at this phase, the movePrimary operation does not recover.
            7120202,
            // In the FSM tests, there is a chance that there are still some User collections
            // left to clone. This occurs when a MovePrimary joins an already existing MovePrimary
            // command that has purposefully triggered a failpoint.
            9046501,
            // Suites with add remove shard might make the move primary to fail with ShardNotFound.
            ErrorCodes.ShardNotFound,
        ],
    };

    let states = {
        create: function (db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);
            const fullNs = coll.getFullName();

            jsTestLog("Executing create state: " + fullNs);
            assert.commandWorked(db.adminCommand({shardCollection: fullNs, key: {_id: 1}, unique: false}));
        },
        drop: function (db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);

            jsTestLog("Executing drop state: " + coll.getFullName());

            assert.commandWorked(db.runCommand({drop: coll.getName()}));
        },
        rename: function (db, collName, connCache) {
            db = this.getRandomDb(db);
            const srcColl = this.getRandomCollection(db);
            const srcCollName = srcColl.getFullName();
            const destCollNS = this.getRandomCollection(db).getFullName();
            const destCollName = destCollNS.split(".")[1];

            jsTestLog("Executing rename state:" + srcCollName + " to " + destCollNS);
            assert.commandWorkedOrFailedWithCode(srcColl.renameCollection(destCollName, true /* dropTarget */), [
                ErrorCodes.NamespaceNotFound,
                ErrorCodes.ConflictingOperationInProgress,
                ErrorCodes.IllegalOperation,
            ]);
        },
        movePrimary: function (db, collName, connCache) {
            db = this.getRandomDb(db);
            const shardId = this.getRandomShard(connCache);

            jsTestLog("Executing movePrimary state: " + db.getName() + " to " + shardId);
            const res = db.adminCommand({movePrimary: db.getName(), to: shardId});
            assert.commandWorkedOrFailedWithCode(res, this.kMovePrimaryAllowedErrorCodes);
        },
        collMod: function (db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);

            jsTestLog("Executing collMod state: " + coll.getFullName());
            assert.commandWorkedOrFailedWithCode(db.runCommand({collMod: coll.getName(), validator: {a: {$gt: 0}}}), [
                ErrorCodes.NamespaceNotFound,
                ErrorCodes.ConflictingOperationInProgress,
            ]);
        },
        checkDatabaseMetadataConsistency: function (db, collName, connCache) {
            db = this.getRandomDb(db);
            jsTestLog("Executing checkMetadataConsistency state for database: " + db.getName());
            const inconsistencies = db.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        checkCollectionMetadataConsistency: function (db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);
            jsTestLog("Executing checkMetadataConsistency state for collection: " + coll.getFullName());
            const inconsistencies = coll.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        untrackUnshardedCollection: function untrackUnshardedCollection(db, collName, connCache) {
            // SERVER-111231 Remove early exit when untrackUnshardedCollection is re-enabled.
            if (TestData.runningWithConfigStepdowns || TestData.runningWithShardStepdowns || TestData.killShards) {
                jsTestLog("Skipping untrackUnshardedCollection as stepdowns are enabled");
                return;
            }

            // Note this command will behave as no-op in case the collection is not tracked.
            db = data.getRandomDb(db);
            const coll = data.getRandomCollection(db);
            const namespace = coll.getFullName();
            jsTestLog(`Started to untrack collection ${namespace}`);
            // Attempt to unshard the collection first
            jsTestLog(`1. Attempting to unshard collection ${namespace}`);
            assert.commandWorkedOrFailedWithCode(db.adminCommand({unshardCollection: namespace}), [
                // Handles the case where the collection/db does not exist
                ErrorCodes.NamespaceNotFound,
                // Handles the case where another resharding operation is in progress
                ErrorCodes.ConflictingOperationInProgress,
                ErrorCodes.ReshardCollectionInProgress,
                // The command is sent while a FCV transition is in progress
                ErrorCodes.CommandNotSupported,
                ErrorCodes.ReshardCollectionAborted,
                // The command is sent while a node is undergoing initial sync
                ErrorCodes.SnapshotTooOld,
            ]);
            jsTestLog(`Unsharding completed ${namespace}`);
            jsTestLog(`2. Untracking collection ${namespace}`);
            // Note this command will behave as no-op in case the collection is not tracked.
            assert.commandWorkedOrFailedWithCode(db.adminCommand({untrackUnshardedCollection: namespace}), [
                // Handles the case where the collection is not located on its primary
                ErrorCodes.OperationFailed,
                // Handles the case where the collection is sharded
                ErrorCodes.InvalidNamespace,
                // Handles the case where the collection/db does not exist
                ErrorCodes.NamespaceNotFound,
            ]);
            jsTestLog(`Untrack collection completed`);
        },
    };

    let setup = function (db, collName, cluster) {
        for (let i = 0; i < this.dbCount; i++) {
            const dbName = this.dbPrefix + i;
            const newDb = db.getSiblingDB(dbName);
            newDb.adminCommand({enablesharding: dbName});
        }
    };

    let teardown = function (db, collName, cluster) {
        const configDB = db.getSiblingDB("config");
        // All the DDLs executed within the context of this workload should have completed, unblocking migrations on each targeted namespace.
        // Allow some grace time for operations issued by background hooks (or the balancer) that might still be inflight.
        assert.soon(
            () => {
                return configDB.collections.countDocuments({allowMigrations: {$exists: true}}) === 0;
            },
            `Found unexpected "allowMigration" field on one or more tracked collections ${tojson(configDB.collections.findOne({allowMigrations: {$exists: true}}))}`,
        );
    };

    return {
        threadCount: 12,
        iterations: 64,
        startState: "create",
        data: data,
        states: states,
        transitions: uniformDistTransitions(states),
        setup: setup,
        teardown: teardown,
        passConnectionCache: true,
    };
})();
