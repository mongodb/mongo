'use strict';

/**
 * view_catalog.js
 *
 * Creates, modifies and drops view namespaces concurrently. Each worker operates on their own view,
 * built on a shared underlying collection.
 */

var $config = (function() {

    var data = {
        // Use the workload name as a prefix for the view name,
        // since the workload name is assumed to be unique.
        prefix: 'view_catalog'
    };

    var states = (function() {

        function init(db, collName) {
            this.threadCollName = db[collName].getName();
            this.threadViewName = this.prefix + '_' + this.tid;
            this.counter = 0;
        }

        function create(db, collName) {
            this.counter++;
            let pipeline = [{$match: {a: this.counter}}];
            assertAlways.commandWorked(
                db.createView(this.threadViewName, this.threadCollName, pipeline));
            confirmViewDefinition(db, this.threadViewName, collName, pipeline);
        }

        function modify(db, collName) {
            this.counter++;
            let pipeline = [{$match: {a: this.counter}}];
            assertAlways.commandWorked(db.runCommand(
                {collMod: this.threadViewName, viewOn: this.threadCollName, pipeline: pipeline}));
            confirmViewDefinition(db, this.threadViewName, collName, pipeline);
        }

        function drop(db, collName) {
            assertAlways.commandWorked(db.runCommand({drop: this.threadViewName}));

            let res = db.runCommand({listCollections: 1, filter: {name: this.threadViewName}});
            assertAlways.commandWorked(res);
            assertAlways.eq(0, res.cursor.firstBatch.length, tojson(res));
        }

        function confirmViewDefinition(db, viewName, collName, pipeline) {
            let res = db.runCommand({listCollections: 1, filter: {name: viewName}});
            assertAlways.commandWorked(res);
            assertAlways.eq(1, res.cursor.firstBatch.length, tojson(res));
            assertAlways.eq({
                name: viewName,
                type: "view",
                options: {viewOn: collName, pipeline: pipeline},
                info: {readOnly: true}
            },
                            res.cursor.firstBatch[0],
                            tojson(res));
        }

        return {init: init, create: create, modify: modify, drop: drop};

    })();

    var transitions = {
        init: {create: 1},
        create: {modify: 0.75, drop: 0.25},
        modify: {modify: 0.5, drop: 0.5},
        drop: {create: 1}
    };

    // This test performs createCollection concurrently from many threads, and createCollection on a
    // sharded cluster takes a distributed lock. Since a distributed lock is acquired by repeatedly
    // attempting to grab the lock every half second for 20 seconds (a max of 40 attempts), it's
    // possible that some thread will be starved by the other threads and fail to grab the lock
    // after 40 attempts. To reduce the likelihood of this, we choose threadCount and iterations so
    // that threadCount * iterations < 40.
    // The threadCount and iterations can be increased once PM-697 ("Remove all usages of
    // distributed lock") is complete.

    return {
        threadCount: 5,
        iterations: 5,
        data: data,
        states: states,
        transitions: transitions,
    };

})();
