'use strict';

/**
 * Concurrently performs DDL commands and verifies guarantees are not broken.
 *
 * @tags: [
 *   requires_fcv_50,
 *   requires_sharding,
 *   requires_fcv_50,
 *   # TODO (SERVER-56879): Support add/remove shards in new DDL paths
 *   does_not_support_add_remove_shards,
 *   featureFlagShardingFullDDLSupport,
 *  ]
 */

const dbPrefix = 'fsmDB_';
const dbCount = 2;
const collPrefix = 'sharded_coll_';
const collCount = 2;

function getRandomDb(db) {
    return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
}

function getRandomCollection(db) {
    return db[collPrefix + Random.randInt(collCount)];
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

            // Rename collection
            const destCollNS = getRandomCollection(db).getFullName();
            const destCollName = destCollNS.split('.')[1];
            jsTestLog('Executing rename state:' + srcCollName + ' to ' + destCollNS);

            assertAlways.commandWorkedOrFailedWithCode(
                srcColl.renameCollection(destCollName, true /* dropTarget */), [
                    ErrorCodes.NamespaceNotFound,
                    ErrorCodes.ConflictingOperationInProgress,
                    ErrorCodes.IllegalOperation
                ]);
        }
    };

    let setup = function(db, collName, connCache) {
        for (var i = 0; i < dbCount; i++) {
            const dbName = dbPrefix + i;
            const newDb = db.getSiblingDB(dbName);
            newDb.adminCommand({enablesharding: dbName});
        }
    };

    let teardown = function(db, collName, cluster) {};

    let transitions = {
        create: {create: 0.33, drop: 0.33, rename: 0.34},
        drop: {create: 0.34, drop: 0.33, rename: 0.33},
        rename: {create: 0.33, drop: 0.34, rename: 0.33}
    };

    return {
        threadCount: 12,
        iterations: 64,
        startState: 'create',
        states: states,
        transitions: transitions,
        data: {},
        setup: setup,
        teardown: teardown,
        passConnectionCache: true
    };
})();
