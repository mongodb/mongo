'use strict';

/**
 * view_catalog.js
 *
 * Creates, modifies and drops view namespaces concurrently. Each worker operates on their own view,
 * built on a shared underlying collection.
 */

load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropCollections

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

    function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + this.prefix + '_\\d+$');
        dropCollections(db, pattern);
    }

    return {
        threadCount: 10,
        iterations: 100,
        data: data,
        states: states,
        transitions: transitions,
        teardown: teardown
    };

})();
