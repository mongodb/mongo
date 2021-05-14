/**
 * Repeatedly creates and drops a database.
 *
 * @tags: [
 *   requires_sharding,
 *   featureFlagShardingFullDDLSupport,
 * ]
 */
'use strict';

const dbPrefix = 'fsmDB_';
const dbCount = 2;
const collPrefix = 'sharded_coll_';
const collCount = 2;

function getRandomDb(db) {
    return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
}

function getRandomCollection(db) {
    return getRandomDb(db)[collPrefix + Random.randInt(collCount)];
}

var $config = (function() {
    var states = (function() {
        function init(db, collName) {
        }

        function enableSharding(db, collName) {
            let myDb = getRandomDb(db);
            jsTestLog('Executing enableSharding state: ' + myDb.getName());
            assert.commandWorked(myDb.adminCommand({enableSharding: myDb.getName()}));
        }

        function shardCollection(db, collName) {
            let coll = getRandomCollection(db);
            jsTestLog('Executing shardCollection state: ' + coll.getFullName());
            assertAlways.commandWorkedOrFailedWithCode(
                db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}),
                [ErrorCodes.NamespaceNotFound]);
        }

        function dropDatabase(db, collName) {
            let myDb = getRandomDb(db);
            jsTestLog('Executing dropDatabase state: ' + myDb.getName());
            assertAlways.commandWorked(myDb.dropDatabase());
        }

        return {
            init: init,
            enableSharding: enableSharding,
            shardCollection: shardCollection,
            dropDatabase: dropDatabase,
        };
    })();

    var transitions = {
        init: {enableSharding: 0.35, dropDatabase: 0.35, shardCollection: 0.3},
        enableSharding: {enableSharding: 0.35, dropDatabase: 0.35, shardCollection: 0.3},
        dropDatabase: {enableSharding: 0.35, dropDatabase: 0.35, shardCollection: 0.3},
        shardCollection: {enableSharding: 0.35, dropDatabase: 0.35, shardCollection: 0.3},
    };

    let teardown = function(db, collName, cluster) {};

    return {
        threadCount: 12,
        iterations: 64,
        startState: 'init',
        data: {},
        states: states,
        transitions: transitions,
        teardown: teardown
    };
})();
