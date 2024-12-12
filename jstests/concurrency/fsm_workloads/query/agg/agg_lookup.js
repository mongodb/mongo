/**
 * agg_lookup.js
 *
 * Runs a $lookup aggregation simultaneously with updates.
 *
 * TODO SERVER-90385 Enable this test in embedded router suites
 * @tags: [
 *   temp_disabled_embedded_router_uncategorized,
 * ]
 */
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {TxnUtil} from "jstests/libs/txns/txn_util.js";

export const $config = (function() {
    const data = {numDocs: 100};

    const states = (function() {
        function query(db, collName) {
            if (this.shouldSkipTest) {
                return;
            }
            // Run the aggregate with 'allowDiskUse' if it was configured during setup.
            const aggOptions = {allowDiskUse: this.allowDiskUse};

            function getQueryResults() {
                let arr = null;
                try {
                    const cursor = db[collName]
                          .aggregate([
                              {
                                  $lookup: {
                                      from: collName,
                                      localField: "_id",
                                      foreignField: "to",
                                      as: "out",
                                  }
                              },
                          ], aggOptions);

                    arr = cursor.toArray();
                } catch (e) {
                    if (TxnUtil.isTransientTransactionError(e)) {
                        throw e;
                    }
                    if (TestData.runningWithShardStepdowns) {
                        // When running with stepdowns, we expect to sometimes see the query
                        // killed.
                        assert.contains(e.code, interruptedQueryErrors);
                    } else {
                        throw e;
                    }
                }

                return arr;
            }

            const res = getQueryResults();
            if (res) {
                assert.eq(res.length, data.numDocs);
            }
        }

        function update(db, collName) {
            if (this.shouldSkipTest) {
                return;
            }
            const index = Random.randInt(this.numDocs + 1);
            const update = Random.randInt(this.numDocs + 1);
            const res = db[collName].update({_id: index}, {$set: {to: update}});
            assert.commandWorked(res);
        }

        return {query, update};
    })();

    const transitions = {query: {query: 0.5, update: 0.5}, update: {query: 0.5, update: 0.5}};

    function setup(db, collName, cluster) {
        // TODO SERVER-88936: Remove this field and associated checks once the flag is active on
        // last-lts.
        this.shouldSkipTest = TestData.runInsideTransaction && cluster.isSharded() &&
            !FeatureFlagUtil.isPresentAndEnabled(db.getMongo(), 'AllowAdditionalParticipants');
        if (this.shouldSkipTest) {
            return;
        }

        // Load example data.
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, to: i + 1});
        }

        const res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);
        assert.eq(this.numDocs, db[collName].find().itcount());

        const getParam = db.adminCommand({getParameter: 1, internalQueryFrameworkControl: 1});
        const isLookupPushdownEnabled = getParam.hasOwnProperty("internalQueryFrameworkControl") &&
            getParam.internalQueryFrameworkControl.value != "forceClassicEngine";

        this.allowDiskUse = true;
        // If $lookup pushdown into SBE is enabled, we select a random join algorithm to use and
        // set the collection up accordingly.
        if (isLookupPushdownEnabled) {
            // Use a random join algorithm on each test run.
            const numStrategies = 3;
            const strategy = Random.randInt(numStrategies);
            if (strategy === 0) {
                jsTestLog("Using hash join");
            } else if (strategy === 1) {
                assert.commandWorked(db[collName].createIndex({to: 1}));
                jsTestLog("Using index join");
                this.allowDiskUse = false;
            } else {
                jsTestLog("Using nested loop join");
                this.allowDiskUse = false;
            }
        }
    }

    function teardown(db, collName) {
        // Drop indexes, if any were created.
        assert.commandWorked(db[collName].dropIndexes());
    }

    return {
        threadCount: 10,
        iterations: 100,
        states: states,
        startState: 'query',
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();
