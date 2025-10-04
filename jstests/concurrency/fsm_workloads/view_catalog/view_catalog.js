/**
 * view_catalog.js
 *
 * Creates, modifies and drops view namespaces concurrently. Each worker operates on their own view,
 * built on a shared underlying collection.
 */
export const $config = (function () {
    let data = {
        // Use the workload name as a prefix for the view name, since the workload name is assumed
        // to be unique.
        prefix: "view_catalog",
    };

    let states = (function () {
        function init(db, collName) {
            this.threadCollName = db[collName].getName();
            this.threadViewName = this.prefix + "_" + this.tid;
            this.counter = 0;
            this.confirmViewDefinition = function confirmViewDefinition(db, viewName, collName, pipeline, counter) {
                assert.eq({_id: counter}, db[collName].aggregate(pipeline).toArray()[0]);
                assert.eq({_id: counter}, db[viewName].findOne());
                const res = db.runCommand({listCollections: 1, filter: {name: viewName}});
                assert.commandWorked(res);
                assert.eq(1, res.cursor.firstBatch.length, tojson(res));
                assert.eq(
                    {
                        name: viewName,
                        type: "view",
                        options: {viewOn: collName, pipeline: pipeline},
                        info: {readOnly: true},
                    },
                    res.cursor.firstBatch[0],
                    tojson(res),
                );
            };
        }

        function create(db, collName) {
            this.counter++;
            let pipeline = [{$match: {_id: this.counter}}];
            assert.commandWorked(db.createView(this.threadViewName, this.threadCollName, pipeline));
            this.confirmViewDefinition(db, this.threadViewName, collName, pipeline, this.counter);
        }

        function modify(db, collName) {
            this.counter++;
            let pipeline = [{$match: {_id: this.counter}}];
            assert.commandWorked(
                db.runCommand({collMod: this.threadViewName, viewOn: this.threadCollName, pipeline: pipeline}),
            );
            this.confirmViewDefinition(db, this.threadViewName, collName, pipeline, this.counter);
        }

        function drop(db, collName) {
            assert.commandWorked(db.runCommand({drop: this.threadViewName}));

            let res = db.runCommand({listCollections: 1, filter: {name: this.threadViewName}});
            assert.commandWorked(res);
            assert.eq(0, res.cursor.firstBatch.length, tojson(res));
        }

        return {init: init, create: create, modify: modify, drop: drop};
    })();

    let transitions = {
        init: {create: 1},
        create: {modify: 0.75, drop: 0.25},
        modify: {modify: 0.5, drop: 0.5},
        drop: {create: 1},
    };

    let setup = function setup(db, collName, cluster) {
        let bulk = db[collName].initializeOrderedBulkOp();
        for (let i = 0; i < this.iterations; i++) {
            bulk.insert({_id: i});
        }
        assert.commandWorked(bulk.execute());
    };

    return {
        threadCount: 10,
        iterations: 10,
        data: data,
        setup: setup,
        states: states,
        transitions: transitions,
    };
})();
