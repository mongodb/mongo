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

load('jstests/libs/feature_flag_util.js');

const dbPrefix = jsTestName() + '_DB_';
const dbCount = 2;
const collPrefix = 'sharded_coll_';
const collCount = 2;

function getRandomDb(db) {
    return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
}

function getRandomCollection(db) {
    return db[collPrefix + Random.randInt(collCount)];
}

function getRandomShard(connCache) {
    const shards = Object.keys(connCache.shards);
    return shards[Random.randInt(shards.length)];
}

var $config = (function() {
    let states = {
        create: function(db, collName, connCache) {
            db = getRandomDb(db);
            const coll = getRandomCollection(db);
            const fullNs = coll.getFullName();

            jsTestLog('Executing create state: ' + fullNs);
            assertAlways.commandWorked(
                db.adminCommand({shardCollection: fullNs, key: {_id: 1}, unique: false}));
        },
        drop: function(db, collName, connCache) {
            db = getRandomDb(db);
            const coll = getRandomCollection(db);

            jsTestLog('Executing drop state: ' + coll.getFullName());
            assertAlways.eq(coll.drop(), true);
        },
        rename: function(db, collName, connCache) {
            db = getRandomDb(db);
            const srcColl = getRandomCollection(db);
            const srcCollName = srcColl.getFullName();
            const destCollNS = getRandomCollection(db).getFullName();
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

            db = getRandomDb(db);
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
            db = getRandomDb(db);
            const coll = getRandomCollection(db);

            jsTestLog('Executing collMod state: ' + coll.getFullName());
            assertAlways.commandWorkedOrFailedWithCode(
                db.runCommand({collMod: coll.getName(), validator: {a: {$gt: 0}}}),
                [ErrorCodes.NamespaceNotFound, ErrorCodes.ConflictingOperationInProgress]);
        }
    };

    let setup = function(db, collName, connCache) {
        // TODO (SERVER-71309): Remove once 7.0 becomes last LTS. Prevent non-resilient movePrimary
        // operations from being executed in multiversion suites.
        this.skipMovePrimary = !FeatureFlagUtil.isEnabled(db.getMongo(), 'ResilientMovePrimary');

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

    let transitions = {
        create: {create: 0.25, drop: 0.25, rename: 0.25, movePrimary: 0.25},
        drop: {create: 0.25, drop: 0.25, rename: 0.25, movePrimary: 0.25},
        rename: {create: 0.25, drop: 0.25, rename: 0.25, movePrimary: 0.25},
        movePrimary: {create: 0.25, drop: 0.25, rename: 0.25, movePrimary: 0.25}
    };

    return {
        threadCount: 12,
        iterations: 64,
        startState: 'create',
        states: states,
        transitions: transitions,
        data: {skipMovePrimary: false},
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
