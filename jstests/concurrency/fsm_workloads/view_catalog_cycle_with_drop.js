'use strict';

/**
 * view_catalog_cycle_with_drop.js
 *
 * Creates a set of views and then attempts to read while remapping views against each other and the
 * underlying collection.
 */

load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropCollections

var $config = (function() {

    // Use the workload name as a prefix for the view names, since the workload name is assumed
    // to be unique.
    const prefix = 'view_catalog_cycle_with_drop_';

    var data = {
        viewList: ['viewA', 'viewB', 'viewC'].map(viewName => prefix + viewName),
        assertCommandWorkedOrFailedWithCode: function(result, codeArr) {
            assertAlways(result.ok === 1 || codeArr.indexOf(result.code) > -1, tojson(result));
        },
        getRandomView: function(viewList) {
            return viewList[Random.randInt(viewList.length)];
        },
    };

    var states = (function() {
        /**
         * Redefines a view definition by changing the namespace it is a view on. We intentionally
         * allow attempting to remap a view to be defined on itself (results in 'GraphContainsCycle'
         * error). We also handle errors for when the view to modify has been dropped by another
         * thread (results in 'NamespaceNotFound' error).
         */
        function remapViewToView(db, collName) {
            const fromName = this.getRandomView(this.viewList);
            const toName = this.getRandomView(this.viewList);
            const res = db.runCommand({collMod: fromName, viewOn: toName, pipeline: []});
            this.assertCommandWorkedOrFailedWithCode(
                res, [ErrorCodes.GraphContainsCycle, ErrorCodes.NamespaceNotFound]);
        }

        /**
         * Drops a view and then recreates against an underlying collection. We handle errors for
         * when the view to drop has already been dropped by another thread and for when the view
         * we want to create has already been created by another thread.
         */
        function recreateViewOnCollection(db, collName) {
            const viewName = this.getRandomView(this.viewList);
            this.assertCommandWorkedOrFailedWithCode(db.runCommand({drop: viewName}),
                                                     [ErrorCodes.NamespaceNotFound]);
            this.assertCommandWorkedOrFailedWithCode(db.createView(viewName, collName, []),
                                                     [ErrorCodes.NamespaceExists]);
        }

        /**
         * Performs a find against a view. We expect that the find command will never fail due to
         * cycle detection (which should be handled at create/modification time). We handle errors
         * for the case where view drop/recreate leads to an attempt by aggregation to read
         * documents directly from the view, rather than the expected collection namespace.
         */
        function readFromView(db, collName) {
            const viewName = this.getRandomView(this.viewList);
            const res = db.runCommand({find: viewName});
            // TODO SERVER-26037: Replace with the appropriate error code. See ticket for details.
            this.assertCommandWorkedOrFailedWithCode(res, [ErrorCodes.CommandNotSupportedOnView]);
        }

        return {
            remapViewToView: remapViewToView,
            recreateViewOnCollection: recreateViewOnCollection,
            readFromView: readFromView
        };

    })();

    var transitions = {
        remapViewToView:
            {remapViewToView: 0.50, recreateViewOnCollection: 0.25, readFromView: 0.25},
        recreateViewOnCollection:
            {remapViewToView: 0.50, recreateViewOnCollection: 0.25, readFromView: 0.25},
        readFromView: {remapViewToView: 0.50, recreateViewOnCollection: 0.25, readFromView: 0.25},
    };

    function setup(db, collName, cluster) {
        let coll = db[collName];
        assertAlways.writeOK(coll.insert({x: 1}));

        for (let viewName of this.viewList) {
            assert.commandWorked(db.createView(viewName, collName, []));
        }
    }

    function teardown(db, collName, cluster) {
        const pattern = new RegExp('^' + prefix + '[A-z]*$');
        dropCollections(db, pattern);
    }

    return {
        threadCount: 10,
        iterations: 100,
        data: data,
        states: states,
        startState: 'readFromView',
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };

})();
