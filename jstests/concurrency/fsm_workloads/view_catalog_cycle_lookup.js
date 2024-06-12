/**
 * view_catalog_cycle_lookup.js
 *
 * Creates views which may include $lookup and $graphlookup stages and continually remaps those
 * views against other eachother and the underlying collection. We are looking to expose situations
 * where a $lookup or $graphLookup view that forms a cycle is created successfully.
 *
 * TODO SERVER-90385 Enable this test in embedded router suites
 * @tags: [
 *     requires_fcv_51,
 *     temp_disabled_embedded_router_uncategorized,
 * ]
 */

import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {TransactionsUtil} from "jstests/libs/transactions_util.js";

export const $config = (function() {
    // Use the workload name as a prefix for the view names, since the workload name is assumed
    // to be unique.
    const prefix = 'view_catalog_cycle_lookup_';

    var data = {
        viewList: ['viewA', 'viewB', 'viewC', 'viewD', 'viewE'].map(viewName => prefix + viewName),
        getRandomView: function(viewList) {
            return viewList[Random.randInt(viewList.length)];
        },
        getRandomViewPipeline:
            function() {
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
                        assert(false, "Invalid index: " + index);
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
            if (this.shouldSkipTest) {
                return;
            }
            if (this.isSBELookupEnabled) {
                return;
            }

            const fromName = this.getRandomView(this.viewList);
            const toName = this.getRandomView(this.viewList);
            const res = db.runCommand(
                {collMod: fromName, viewOn: toName, pipeline: this.getRandomViewPipeline()});
            assert(res.ok === 1 ||
                       [ErrorCodes.GraphContainsCycle, ErrorCodes.ConflictingOperationInProgress]
                           .includes(res.code),
                   tojson(res));
        }

        /**
         * Redefines a view definition by changing 'viewOn' to the underlying collection. This may
         * lead to a failed command if the given collMod would introduce a cycle. We ignore this
         * error as it is expected at view create/modification time.
         */
        function remapViewToCollection(db, collName) {
            if (this.shouldSkipTest) {
                return;
            }
            if (this.isSBELookupEnabled) {
                return;
            }

            const fromName = this.getRandomView(this.viewList);
            const res = db.runCommand(
                {collMod: fromName, viewOn: collName, pipeline: this.getRandomViewPipeline()});
            assert(res.ok === 1 ||
                       [ErrorCodes.GraphContainsCycle, ErrorCodes.ConflictingOperationInProgress]
                           .includes(res.code),
                   tojson(res));
        }

        function readFromView(db, collName) {
            if (this.shouldSkipTest) {
                return;
            }
            if (this.isSBELookupEnabled) {
                return;
            }

            const viewName = this.getRandomView(this.viewList);
            const res = db.runCommand({find: viewName});

            // If we encountered some transaction failure propagate this upwards so it gets
            // automatically retried.
            if (TransactionsUtil.isTransientTransactionError(res)) {
                throw res;
            }

            // When initializing an aggregation on a view, the server briefly releases its
            // collection lock before creating and iterating the cursor on the underlying namespace.
            // In this short window of time, it's possible that that namespace has been dropped and
            // replaced with a view.
            //
            // TODO (SERVER-35635): It would be more appropriate for the server to return
            // OperationFailed, as CommandNotSupportedOnView is misleading.
            // TODO SERVER-XXXXX: The mergeCursors sent by mongos to mongod might get killed during
            // a remapping and return a somewhat misleading CursorNotFound error. Ideally it should
            // be remapped to an OperationFailed error.
            assert(res.ok === 1 || res.code === ErrorCodes.CommandNotSupportedOnView ||
                       res.code === ErrorCodes.CursorNotFound ||
                       res.code === ErrorCodes.CommandOnShardedViewNotSupportedOnMongod,
                   () => tojson(res));
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
        // TODO SERVER-88936: Remove this field and associated checks once the flag is active on
        // last-lts.
        this.shouldSkipTest = TestData.runInsideTransaction && cluster.isSharded() &&
            !FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'AllowAdditionalParticipants');
        if (this.shouldSkipTest) {
            return;
        }

        const coll = db[collName];

        assert.commandWorked(coll.insert({a: 1, b: 2}));
        assert.commandWorked(coll.insert({a: 2, b: 3}));
        assert.commandWorked(coll.insert({a: 3, b: 4}));
        assert.commandWorked(coll.insert({a: 4, b: 1}));

        for (let viewName of this.viewList) {
            assert.commandWorked(db.createView(viewName, collName, []));
        }

        // We need to increase the maximum sub-pipeline view depth for this test since sharded view
        // resolution of views with pipelines containing $lookups on other views can result in deep
        // nesting of subpipelines. For the purposes of this test, the limit needs to be higher than
        // the default.
        cluster.executeOnMongodNodes((db) => {
            // Store the old value of the max subpipeline view depth so we can restore it at the end
            // of the test.
            const maxSubPipelineViewDepthParam =
                db.adminCommand({getParameter: 1, internalMaxSubPipelineViewDepth: 1});
            assert(maxSubPipelineViewDepthParam.hasOwnProperty("internalMaxSubPipelineViewDepth"));
            this.oldMaxSubPipelineViewDepth =
                maxSubPipelineViewDepthParam.internalMaxSubPipelineViewDepth;
            assert.commandWorked(
                db.adminCommand({setParameter: 1, internalMaxSubPipelineViewDepth: 100}));
        });
    }

    function teardown(db, collName, cluster) {
        // Restore the old max subpipeline view depth.
        if (this.oldMaxSubPipelineViewDepth) {
            cluster.executeOnMongodNodes((db) => {
                assert.commandWorked(db.adminCommand({
                    setParameter: 1,
                    internalMaxSubPipelineViewDepth: this.oldMaxSubPipelineViewDepth
                }));
            });
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
        teardown: teardown,
    };
})();
