/**
 * Concurrently performs DDL commands and verifies guarantees are not broken.
 *
 * @tags: [
 *   requires_sharding,
 *  ]
 */

import {
    uniformDistTransitions
} from "jstests/concurrency/fsm_workload_helpers/state_transition_utils.js";

const dbPrefix = jsTestName() + '_DB_';
const dbCount = 2;
const collPrefix = 'sharded_coll_';
const collCount = 2;

function getRandomShard(connCache) {
    const shards = Object.keys(connCache.shards);
    return shards[Random.randInt(shards.length)];
}

export const $config = (function() {
    let data = {
        getRandomDb: function(db) {
            return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
        },
        getRandomCollection: function(db) {
            return db[collPrefix + Random.randInt(collCount)];
        },
        movePrimaryAllowedErrorCodes: [
            ErrorCodes.ConflictingOperationInProgress,
            // The cloning phase has failed (e.g. as a result of a stepdown). When a failure
            // occurs at this phase, the movePrimary operation does not recover.
            7120202,
            // In the FSM tests, there is a chance that there are still some User collections
            // left to clone. This occurs when a MovePrimary joins an already existing MovePrimary
            // command that has purposefully triggered a failpoint.
            9046501,
        ]
    };

    let states = {
        create: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);
            const fullNs = coll.getFullName();

            jsTestLog('Executing create state: ' + fullNs);
            assert.commandWorked(
                db.adminCommand({shardCollection: fullNs, key: {_id: 1}, unique: false}));
        },
        drop: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);

            jsTestLog('Executing drop state: ' + coll.getFullName());

            assert.eq(coll.drop(), true);
        },
        rename: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const srcColl = this.getRandomCollection(db);
            const srcCollName = srcColl.getFullName();
            const destCollNS = this.getRandomCollection(db).getFullName();
            const destCollName = destCollNS.split('.')[1];

            jsTestLog('Executing rename state:' + srcCollName + ' to ' + destCollNS);
            assert.commandWorkedOrFailedWithCode(
                srcColl.renameCollection(destCollName, true /* dropTarget */), [
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.ConflictingOperationInProgress,
                    ErrorCodes.IllegalOperation
                ]);
        },
        movePrimary: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const shardId = getRandomShard(connCache);

            jsTestLog('Executing movePrimary state: ' + db.getName() + ' to ' + shardId);
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({movePrimary: db.getName(), to: shardId}),
                this.movePrimaryAllowedErrorCodes);
        },
        collMod: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);

            jsTestLog('Executing collMod state: ' + coll.getFullName());
            assert.commandWorkedOrFailedWithCode(
                db.runCommand({collMod: coll.getName(), validator: {a: {$gt: 0}}}),
                [ErrorCodes.NamespaceNotFound, ErrorCodes.ConflictingOperationInProgress]);
        },
        checkDatabaseMetadataConsistency: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            jsTestLog('Executing checkMetadataConsistency state for database: ' + db.getName());
            const inconsistencies = db.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        checkCollectionMetadataConsistency: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);
            jsTestLog('Executing checkMetadataConsistency state for collection: ' +
                      coll.getFullName());
            const inconsistencies = coll.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        untrackUnshardedCollection: function untrackUnshardedCollection(db, collName, connCache) {
            // Note this command will behave as no-op in case the collection is not tracked.
            const namespace = `${db}.${collName}`;
            jsTestLog(`Started to untrack collection ${namespace}`);
            assert.commandWorkedOrFailedWithCode(
                db.adminCommand({untrackUnshardedCollection: namespace}), [
                    // Handles the case where the collection is not located on its primary
                    ErrorCodes.OperationFailed,
                    // Handles the case where the collection is sharded
                    ErrorCodes.InvalidNamespace,
                    // Handles the case where the collection/db does not exist
                    ErrorCodes.NamespaceNotFound,
                    //  TODO (SERVER-96072) remove this error once the command is backported.
                    ErrorCodes.CommandNotFound,
                ]);
            jsTestLog(`Untrack collection completed`);
        }
    };

    let setup = function(db, collName, cluster) {
        for (var i = 0; i < dbCount; i++) {
            const dbName = dbPrefix + i;
            const newDb = db.getSiblingDB(dbName);
            newDb.adminCommand({enablesharding: dbName});
        }
    };

    let teardown = function(db, collName, cluster) {
        const configDB = db.getSiblingDB("config");
        assert(configDB.collections.countDocuments({allowMigrations: {$exists: true}}) == 0);
    };

    return {
        threadCount: 12,
        iterations: 64,
        startState: 'create',
        data: data,
        states: states,
        transitions: uniformDistTransitions(states),
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
