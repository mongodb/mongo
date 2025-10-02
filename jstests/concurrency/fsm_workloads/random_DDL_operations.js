'use strict';

/**
 * Concurrently performs DDL commands and verifies guarantees are not broken.
 *
 * @tags: [
 *   requires_sharding,
 *   # TODO (SERVER-56879): Support add/remove shards in new DDL paths
 *   does_not_support_add_remove_shards,
 *  ]
 */

load("jstests/concurrency/fsm_workload_helpers/state_transition_utils.js");
load('jstests/libs/feature_flag_util.js');

const dbPrefix = jsTestName() + '_DB_';
const dbCount = 2;
const collPrefix = 'sharded_coll_';
const collCount = 2;

function getRandomShard(connCache) {
    const shards = Object.keys(connCache.shards);
    return shards[Random.randInt(shards.length)];
}

var $config = (function() {
    let data = {
        getRandomDb: function(db) {
            return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
        },
        getRandomCollection: function(db) {
            return db[collPrefix + Random.randInt(collCount)];
        },
    };

    let states = {
        create: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);
            const fullNs = coll.getFullName();

            jsTestLog('Executing create state: ' + fullNs);
            assertAlways.commandWorked(
                db.adminCommand({shardCollection: fullNs, key: {_id: 1}, unique: false}));
        },
        drop: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);

            jsTestLog('Executing drop state: ' + coll.getFullName());

            assert.commandWorked(db.runCommand({drop: coll.getName()}));
        },
        rename: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const srcColl = this.getRandomCollection(db);
            const srcCollName = srcColl.getFullName();
            const destCollNS = this.getRandomCollection(db).getFullName();
            const destCollName = destCollNS.split('.')[1];

            jsTestLog('Executing rename state:' + srcCollName + ' to ' + destCollNS);
            assertAlways.commandWorkedOrFailedWithCode(
                srcColl.renameCollection(destCollName, true /* dropTarget */), [
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.ConflictingOperationInProgress,
                    ErrorCodes.IllegalOperation
                ]);
        },
        movePrimary: function(db, collName, connCache) {
            if (this.skipMovePrimary) {
                return;
            }

            db = this.getRandomDb(db);
            const shardId = getRandomShard(connCache);

            jsTestLog('Executing movePrimary state: ' + db.getName() + ' to ' + shardId);
            assertAlways.commandWorkedOrFailedWithCode(
                db.adminCommand({movePrimary: db.getName(), to: shardId}), [
                    ErrorCodes.ConflictingOperationInProgress,
                    // The cloning phase has failed (e.g. as a result of a stepdown). When a failure
                    // occurs at this phase, the movePrimary operation does not recover.
                    7120202
                ]);
        },
        collMod: function(db, collName, connCache) {
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);

            jsTestLog('Executing collMod state: ' + coll.getFullName());
            assertAlways.commandWorkedOrFailedWithCode(
                db.runCommand({collMod: coll.getName(), validator: {a: {$gt: 0}}}),
                [ErrorCodes.NamespaceNotFound, ErrorCodes.ConflictingOperationInProgress]);
        },
        checkDatabaseMetadataConsistency: function(db, collName, connCache) {
            if (this.skipMetadataChecks) {
                return;
            }
            db = this.getRandomDb(db);
            jsTestLog('Executing checkMetadataConsistency state for database: ' + db.getName());
            const inconsistencies = db.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        },
        checkCollectionMetadataConsistency: function(db, collName, connCache) {
            if (this.skipMetadataChecks) {
                return;
            }
            db = this.getRandomDb(db);
            const coll = this.getRandomCollection(db);
            jsTestLog('Executing checkMetadataConsistency state for collection: ' +
                      coll.getFullName());
            const inconsistencies = coll.checkMetadataConsistency().toArray();
            assert.eq(0, inconsistencies.length, tojson(inconsistencies));
        }
    };

    let setup = function(db, collName, cluster) {
        // TODO (SERVER-71309): Remove once 7.0 becomes last LTS. Prevent non-resilient movePrimary
        // operations from being executed in multiversion suites.
        this.skipMovePrimary = !FeatureFlagUtil.isEnabled(db.getMongo(), 'ResilientMovePrimary');
        this.skipMetadataChecks =
            // TODO SERVER-70396: remove this flag
            !FeatureFlagUtil.isEnabled(db.getMongo(), 'CheckMetadataConsistency');

        for (var i = 0; i < dbCount; i++) {
            const dbName = dbPrefix + i;
            const newDb = db.getSiblingDB(dbName);
            newDb.adminCommand({enablesharding: dbName});
        }
    };

    let teardown = function(db, collName, cluster) {
        const configDB = db.getSiblingDB("config");
        assertAlways(configDB.collections.countDocuments({allowMigrations: {$exists: true}}) == 0);
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
