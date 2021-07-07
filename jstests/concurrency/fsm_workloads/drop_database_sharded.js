/**
 * Repeatedly creates and drops a database.
 *
 * @tags: [
 *   requires_sharding,
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

/*
 * Keep track of dropDB state results: successful dropDatabase or staleDB error.
 */
const logDropDbResultsDBName = 'successful_drop_db';
const logDropDbResultsCollName = 'log';

function logFinishedDropDBState(db, isSuccess) {
    db = db.getSiblingDB(logDropDbResultsDBName);
    const coll = db[logDropDbResultsCollName];
    coll.update({_id: isSuccess ? 'OK' : 'notOK'}, {$inc: {'count': 1}}, {upsert: true});
}

/*
 * At least one dropDatabase must have succeeded. Returns a tuple <number of successful dropDB
 * states, number of dropDB states failed with staleDB exception>
 */
function getDropDbStateResults(db) {
    db = db.getSiblingDB(logDropDbResultsDBName);
    const coll = db[logDropDbResultsCollName];
    const countOK = coll.findOne({_id: 'OK'}).count;
    const notOK = coll.findOne({_id: 'notOK'});
    const countNotOK = notOK ? notOK.count : 0;
    // At least one dropDatabase must have succeeded
    assert.gte(countOK, 1, 'No dropDatabase succeeded, got ' + countNotOK + ' stale db versions');
    return {ok: countOK, notOK: countNotOK};
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
                [ErrorCodes.NamespaceNotFound, ErrorCodes.StaleDbVersion]);
        }

        function dropDatabase(db, collName) {
            let myDb = getRandomDb(db);
            jsTestLog('Executing dropDatabase state: ' + myDb.getName());
            var resOK;
            try {
                assertAlways.commandWorked(myDb.dropDatabase());
                resOK = true;
            } catch (e) {
                resOK = false;
                // StaleDbVersion is the only expected exception for drop database
                if (!e.code || e.code != ErrorCodes.StaleDbVersion) {
                    throw e;
                }
            }
            logFinishedDropDBState(db, resOK);
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

    let teardown = function(db, collName, cluster) {
        const dropDbStatesResults = getDropDbStateResults(db);
        jsTestLog('Finished FSM with ' + dropDbStatesResults.ok + ' successful dropDB and ' +
                  dropDbStatesResults.notOK + ' stale db exceptions');
    };

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
