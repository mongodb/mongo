/**
 * Repeatedly creates and drops a sharded time-series collection.
 *
 * @tags: [
 *   requires_sharding,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_51,
 *   requires_timeseries,
 * ]
 */
const dbPrefix = jsTestName();
const dbCount = 2;
const collPrefix = "sharded_timeseries_collection_";
const collCount = 2;
const timeField = "time";
const metaField = "hostId";

function getRandomDb(db) {
    return db.getSiblingDB(dbPrefix + Random.randInt(dbCount));
}

function getRandomTimeseriesView(db) {
    return getRandomDb(db)[collPrefix + Random.randInt(collCount)];
}

export const $config = (function () {
    const setup = function (db, collName, cluster) {
        // Enable sharding for the test databases.
        for (let i = 0; i < dbCount; i++) {
            const dbName = dbPrefix + i;
            db.adminCommand({enablesharding: dbName});
        }
    };

    const states = {
        init: function (db, collName) {},
        create: function (db, collName) {
            const coll = getRandomTimeseriesView(db);
            jsTestLog("Executing create state on: " + coll.getFullName());
            assert.commandWorked(
                db.adminCommand({
                    shardCollection: coll.getFullName(),
                    key: {[metaField]: 1, [timeField]: 1},
                    timeseries: {timeField: timeField, metaField: metaField},
                }),
            );
        },
        dropView: function (db, collName) {
            const coll = getRandomTimeseriesView(db);
            jsTestLog("Executing dropView state on: " + coll.getFullName());
            assert.commandWorked(coll.getDB().runCommand({drop: coll.getName()}));
        },
    };

    const transitions = {
        init: {create: 0.33, dropView: 0.33},
        create: {create: 0.33, dropView: 0.33},
        dropView: {create: 0.33, dropView: 0.33},
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
