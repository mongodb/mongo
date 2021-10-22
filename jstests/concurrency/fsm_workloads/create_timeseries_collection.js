"use strict";

/**
 * Repeatedly creates a time-series collection, inserts data and drops it.
 *
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 * ]
 */
var $config = (function() {
    var data = {prefix: "create_timeseries_collection"};

    var states = (function() {
        function getCollectionName(prefix, collName, tid) {
            return prefix + "_" + collName + "_" + tid;
        }

        function init(db, collName) {
            this.num = 0;
        }

        function create(db, collName) {
            collName = getCollectionName(this.prefix, collName, this.tid);

            const timeFieldName = "time";
            assertAlways.commandWorked(
                db.createCollection(collName, {timeseries: {timeField: timeFieldName}}));
        }

        function insert(db, collName) {
            collName = getCollectionName(this.prefix, collName, this.tid);

            const coll = db.getCollection(collName);
            assertAlways.commandWorked(coll.insert({
                _id: this.num,
                measurement: "measurement",
                time: ISODate(),
            }));
        }

        function drop(db, collName) {
            collName = getCollectionName(this.prefix, collName, this.tid);
            assertAlways(db.getCollection(collName).drop(), "failed to drop " + collName);
        }

        return {init: init, create: create, insert: insert, drop: drop};
    })();

    var transitions = {
        init: {create: 1},
        create: {insert: 0.8, drop: 0.2},
        insert: {insert: 0.8, drop: 0.2},
        drop: {create: 1}
    };

    return {
        threadCount: 4,
        iterations: 1000,
        data: data,
        states: states,
        transitions: transitions,
    };
})();
