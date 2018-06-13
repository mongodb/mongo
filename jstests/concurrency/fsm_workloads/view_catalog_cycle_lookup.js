'use strict';

/**
 * view_catalog_cycle_lookup.js
 *
 * Creates views which may include $lookup and $graphlookup stages and continually remaps those
 * views against other eachother and the underlying collection. We are looking to expose situations
 * where a $lookup or $graphLookup view that forms a cycle is created successfully.
 */

var $config = (function() {

    // Use the workload name as a prefix for the view names, since the workload name is assumed
    // to be unique.
    const prefix = 'view_catalog_cycle_lookup_';

    var data = {
        viewList: ['viewA', 'viewB', 'viewC', 'viewD', 'viewE'].map(viewName => prefix + viewName),
        getRandomView: function(viewList) {
            return viewList[Random.randInt(viewList.length)];
        },
        getRandomViewPipeline: function() {
            const lookupViewNs1 = this.getRandomView(this.viewList);
            const lookupViewNs2 = this.getRandomView(this.viewList);
            const index = Random.randInt(4);
            switch (index) {
                case 0:
                    return [{
                        $lookup: {
                            from: lookupViewNs1,
                            localField: 'a',
                            foreignField: 'b',
                            as: 'result1'
                        }
                    }];
                case 1:
                    return [{
                        $lookup: {
                            from: lookupViewNs1,
                            let : {a1: '$a'},
                            pipeline: [
                                {$match: {$expr: {$eq: ["$$a1", "$b"]}}},
                                {
                                  $lookup: {
                                      from: lookupViewNs2,
                                      let : {b1: '$b'},
                                      pipeline: [{$match: {$expr: {$eq: ["$$b1", "$b"]}}}],
                                      as: "result2Inner"
                                  }
                                }
                            ],
                            as: 'result2'
                        }
                    }];
                case 2:
                    return [{
                        $graphLookup: {
                            from: lookupViewNs1,
                            startWith: '$a',
                            connectFromField: 'a',
                            connectToField: 'b',
                            as: 'result3'
                        }
                    }];
                case 3:
                    return [];
                default:
                    assertAlways(false, "Invalid index: " + index);
            }
        },
    };

    var states = (function() {
        /**
         * Redefines a view definition by changing the namespace it is a view on. This may lead to
         * a failed command if the given collMod would introduce a cycle. We ignore this error as it
         * is expected at view create/modification time.
         */
        function remapViewToView(db, collName) {
            const fromName = this.getRandomView(this.viewList);
            const toName = this.getRandomView(this.viewList);
            const res = db.runCommand(
                {collMod: fromName, viewOn: toName, pipeline: this.getRandomViewPipeline()});
            assertAlways(res.ok === 1 || res.code === ErrorCodes.GraphContainsCycle, tojson(res));
        }

        /**
         * Redefines a view definition by changing 'viewOn' to the underlying collection. This may
         * lead to a failed command if the given collMod would introduce a cycle. We ignore this
         * error as it is expected at view create/modification time.
         */
        function remapViewToCollection(db, collName) {
            const fromName = this.getRandomView(this.viewList);
            const res = db.runCommand(
                {collMod: fromName, viewOn: collName, pipeline: this.getRandomViewPipeline()});
            assertAlways(res.ok === 1 || res.code === ErrorCodes.GraphContainsCycle, tojson(res));
        }

        function readFromView(db, collName) {
            const viewName = this.getRandomView(this.viewList);
            assertAlways.commandWorked(db.runCommand({find: viewName}));
        }

        return {
            remapViewToView: remapViewToView,
            remapViewToCollection: remapViewToCollection,
            readFromView: readFromView,
        };

    })();

    var transitions = {
        remapViewToView: {remapViewToView: 0.40, remapViewToCollection: 0.10, readFromView: 0.50},
        remapViewToCollection:
            {remapViewToView: 0.40, remapViewToCollection: 0.10, readFromView: 0.50},
        readFromView: {remapViewToView: 0.40, remapViewToCollection: 0.10, readFromView: 0.50},
    };

    function setup(db, collName, cluster) {
        const coll = db[collName];

        assertAlways.writeOK(coll.insert({a: 1, b: 2}));
        assertAlways.writeOK(coll.insert({a: 2, b: 3}));
        assertAlways.writeOK(coll.insert({a: 3, b: 4}));
        assertAlways.writeOK(coll.insert({a: 4, b: 1}));

        for (let viewName of this.viewList) {
            assertAlways.commandWorked(db.createView(viewName, collName, []));
        }
    }

    return {
        threadCount: 20,
        iterations: 100,
        data: data,
        states: states,
        startState: 'readFromView',
        transitions: transitions,
        setup: setup,
    };
})();
