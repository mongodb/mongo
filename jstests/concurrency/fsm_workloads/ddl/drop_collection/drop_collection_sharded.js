/**
 * drop_collection_sharded.js
 *
 * Repeatedly creates and drops a collection.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
const dbPrefix = jsTestName() + "_DB_";
const dbCount = 2;
const collPrefix = "sharded_coll_";
const collCount = 2;

function getRandomDb(db) {
    return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
}

function getRandomCollection(db) {
    return getRandomDb(db)[collPrefix + Random.randInt(collCount)];
}

export const $config = (function () {
    let setup = function (db, collName, cluster) {
        // Initialize databases
        for (let i = 0; i < dbCount; i++) {
            const dbName = dbPrefix + i;
            db.adminCommand({enablesharding: dbName});
        }
    };

    let states = (function () {
        function init(db, collName) {}

        function create(db, collName) {
            const coll = getRandomCollection(db);
            jsTestLog("Executing create state on: " + coll.getFullName());
            assert.commandWorked(db.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));
        }

        function drop(db, collName) {
            const coll = getRandomCollection(db);
            jsTestLog("Executing drop state on: " + coll.getFullName());
            assert.commandWorked(coll.getDB().runCommand({drop: coll.getName()}));
        }

        return {init: init, create: create, drop: drop};
    })();

    let transitions = {
        init: {create: 0.5, drop: 0.5},
        create: {create: 0.5, drop: 0.5},
        drop: {create: 0.5, drop: 0.5},
    };

    return {
        threadCount: 12,
        iterations: 64,
        startState: "init",
        data: {},
        states: states,
        setup: setup,
        transitions: transitions,
    };
})();
