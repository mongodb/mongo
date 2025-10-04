/**
 * view_catalog_cycle_with_drop.js
 *
 * Creates a set of views and then attempts to read while remapping views against each other and the
 * underlying collection.
 */
export const $config = (function () {
    // Use the workload name as a prefix for the view names, since the workload name is assumed
    // to be unique.
    const prefix = "view_catalog_cycle_with_drop_";

    let data = {
        viewList: ["viewA", "viewB", "viewC"].map((viewName) => prefix + viewName),
        getRandomView: function (viewList) {
            return viewList[Random.randInt(viewList.length)];
        },
    };

    let states = (function () {
        /**
         * Redefines a view definition by changing the namespace it is a view on. We intentionally
         * allow attempting to remap a view to be defined on itself (results in 'GraphContainsCycle'
         * error). We also handle errors for when the view to modify has been dropped by another
         * thread (results in 'NamespaceNotFound' error).
         */
        function remapViewToView(db, collName) {
            const fromName = this.getRandomView(this.viewList);
            const toName = this.getRandomView(this.viewList);
            const cmd = {collMod: fromName, viewOn: toName, pipeline: []};
            const res = db.runCommand(cmd);
            const errorCodes = [
                ErrorCodes.GraphContainsCycle,
                ErrorCodes.NamespaceNotFound,
                ErrorCodes.ConflictingOperationInProgress,
            ];
            assert.commandWorkedOrFailedWithCode(res, errorCodes, () => `cmd: ${tojson(cmd)}`);
        }

        /**
         * Drops a view and then recreates against an underlying collection. We handle errors for
         * when the view to drop has already been dropped by another thread and for when the view
         * we want to create has already been created by another thread.
         */
        function recreateViewOnCollection(db, collName) {
            const viewName = this.getRandomView(this.viewList);
            const dropCmd = {drop: viewName};
            let res = db.runCommand(dropCmd);
            let errorCodes = [ErrorCodes.NamespaceNotFound];
            assert.commandWorkedOrFailedWithCode(db.runCommand(dropCmd), errorCodes, () => `cmd: ${tojson(dropCmd)}`);

            res = db.createView(viewName, collName, []);
            errorCodes = [ErrorCodes.NamespaceExists, ErrorCodes.NamespaceNotFound];
            assert.commandWorkedOrFailedWithCode(res, errorCodes, () => `cmd: createView`);
        }

        /**
         * Performs a find against a view. We expect that the find command will never fail due to
         * cycle detection (which should be handled at create/modification time). We handle errors
         * for the case where view drop/recreate leads to an attempt by aggregation to read
         * documents directly from the view, rather than the expected collection namespace.
         */
        function readFromView(db, collName) {
            const viewName = this.getRandomView(this.viewList);
            const cmd = {find: viewName};
            const res = db.runCommand(cmd);
            const errorCodes = [ErrorCodes.CommandNotSupportedOnView];
            // TODO SERVER-26037: Replace with the appropriate error code. See ticket for details.
            assert.commandWorkedOrFailedWithCode(res, errorCodes, () => `cmd: ${tojson(cmd)}`);
        }

        return {
            remapViewToView: remapViewToView,
            recreateViewOnCollection: recreateViewOnCollection,
            readFromView: readFromView,
        };
    })();

    let transitions = {
        remapViewToView: {remapViewToView: 0.5, recreateViewOnCollection: 0.25, readFromView: 0.25},
        recreateViewOnCollection: {remapViewToView: 0.5, recreateViewOnCollection: 0.25, readFromView: 0.25},
        readFromView: {remapViewToView: 0.5, recreateViewOnCollection: 0.25, readFromView: 0.25},
    };

    function setup(db, collName, cluster) {
        let coll = db[collName];
        assert.commandWorked(coll.insert({x: 1}));

        for (let viewName of this.viewList) {
            assert.commandWorked(db.createView(viewName, collName, []));
        }
    }

    return {
        threadCount: 10,
        iterations: 10,
        data: data,
        states: states,
        startState: "readFromView",
        transitions: transitions,
        setup: setup,
    };
})();
