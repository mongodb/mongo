/**
 * Runs a $lookup aggregation simultaneously with updates. The test works by maintaining a 1:1
 * relationship between two collections that gets atomically updated with a transaction. The
 * atomicity is validated by having another transaction using snapshot isolation verify the
 * consistency.
 *
 * The relationship consists of two collections that create a cycle. That is:
 *
 * - coll {_id: 1, to: 3} -> coll_aux {_id: 3, to: 1} -> coll {_id: 1, to: 3}
 *
 * Any inconsistencies would mean that snapshot transactions do not work properly as the cycle gets
 * updated transactionally and no leftover documents in coll_aux can exist.
 *
 * TODO SERVER-90385 Enable this test in embedded router suites
 * @tags: [
 *     uses_transactions,
 *     assumes_snapshot_transactions,
 *     requires_fcv_80,
 *     temp_disabled_embedded_router_uncategorized,
 *     assumes_balancer_off,
 * ]
 * ]
 */
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {
    withTxnAndAutoRetry
} from "jstests/concurrency/fsm_workload_helpers/auto_retry_transaction.js";
import {TxnUtil} from "jstests/libs/txns/txn_util.js";

export const $config = (function() {
    const data = {
        numDocs: 500,  // > 101 so that we force getMore operations.
        shardKey: {_id: 'hashed'},
    };

    function getAuxiliaryCollection(collName) {
        return `${collName}_aux`;
    }

    const states = (function() {
        function query(db, collName) {
            // Run the aggregate with 'allowDiskUse' if it was configured during setup.
            const aggOptions = {allowDiskUse: this.allowDiskUse};

            function getQueryResults(collA, collB) {
                let arr = null;
                try {
                    const cursor = collA
                          .aggregate([
                              {
                                  $lookup: {
                                      from: collB.getName(),
                                      localField: "to",
                                      foreignField: "_id",
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

            withTxnAndAutoRetry(this.session, () => {
                const coll_aux = this.session.getDatabase(db.getName())
                                     .getCollection(getAuxiliaryCollection(collName));
                const coll = this.session.getDatabase(db.getName()).getCollection(collName);

                // We analyze the cycle from one direction or another so that we exercise more
                // potential routes.
                const forwardDirection = Random.rand() > 0.5;
                let res = null;
                const directionString = forwardDirection ? "coll -> coll_aux" : "coll_aux -> coll";
                if (!forwardDirection) {
                    res = getQueryResults(coll_aux, coll);
                } else {
                    res = getQueryResults(coll, coll_aux);
                }

                if (res) {
                    assert.eq(res.length, data.numDocs);
                    for (let i = 0; i < res.length; i++) {
                        // Only one element must be present as we maintain a 1:1 relationship
                        // between foreign and local collection.
                        const doc = res[i];
                        assert.eq(doc.out.length,
                                  1,
                                  directionString +
                                      `: Failed consistency, document returned was ${
                                          JSON.stringify(doc)}`);
                        assert.eq(doc._id,
                                  doc.out[0].to,
                                  directionString +
                                      `: Failed consistency, document returned was ${
                                          JSON.stringify(doc)}`);
                    }
                }
            });
        }

        function update(db, collName) {
            // This transaction updates the cycle mentioned in the header. To do so we
            // transactionally perform the following for a given document
            //   coll {_id: <id>, to: <to>}:
            //
            // * Delete document {_id: <to>} from coll_aux
            // * Insert new document in coll_aux {_id: <new id>, to: <id>}
            // * Update the document in coll to {_id: <id>, to: <new id>}
            //
            // This essentially creates a new cycle while removing the old one, leaving no loose
            // ends.
            withTxnAndAutoRetry(this.session, () => {
                const coll = this.session.getDatabase(db.getName()).getCollection(collName);
                const coll_aux = this.session.getDatabase(db.getName())
                                     .getCollection(getAuxiliaryCollection(collName));
                const idToModify = Random.randInt(this.numDocs);
                const docToModify = coll.findOne({_id: idToModify});

                let res = coll_aux.deleteOne({_id: docToModify.to});
                assert.commandWorked(res);
                assert.eq(res.deletedCount,
                          1,
                          `Failed to delete associated doc to {_id: ${idToModify}, to: ${
                              docToModify.to}}, received: ${JSON.stringify(res)}`);
                res = coll_aux.insertOne({to: idToModify});
                assert.commandWorked(res);
                res = coll.update({_id: idToModify}, {$set: {to: res.insertedId}});
                assert.commandWorked(res);
                assert.eq(res.nModified,
                          1,
                          `Failed to update doc with {_id: ${idToModify}}, received: ${
                              JSON.stringify(res)}`);
            });
        }

        function init(db, collName) {
            this.session = db.getMongo().startSession({causalConsistency: true});
        }

        return {init, query, update};
    })();

    const transitions = {
        init: {query: 0.5, update: 0.5},
        query: {query: 0.5, update: 0.5},
        update: {query: 0.5, update: 0.5}
    };

    function setup(db, collName, cluster) {
        // TODO SERVER-89663: As part of the design for additional transaction participants we were
        // willing to accept leaving open some transactions in case of abort/commit. These read-only
        // transactions are expected to be reaped by the transaction reaper to avoid deadlocking the
        // server since they will hold locks. We lower the value to the default 60 seconds since
        // otherwise it will be 24 hours during testing.
        this.originalTransactionLifetimeLimitSeconds = {};
        cluster.executeOnMongodNodes((db) => {
            const res = assert.commandWorked(
                db.adminCommand({setParameter: 1, transactionLifetimeLimitSeconds: 60}));
            this.originalTransactionLifetimeLimitSeconds[db.getMongo().host] = res.was;
        });

        // Load example data.
        const bulk = db[collName].initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            bulk.insert({_id: i, to: i});
        }

        let res = bulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);
        assert.eq(this.numDocs, db[collName].find().itcount());

        const coll_aux = db[getAuxiliaryCollection(collName)];
        coll_aux.createIndex({_id: 1});
        const mustShardForeignCollection = cluster.isSharded() && Random.rand() > 0.5;
        if (mustShardForeignCollection) {
            jsTest.log("Sharding auxiliary collection");
            cluster.shardCollection(coll_aux, this.shardKey, false);
        } else {
            jsTest.log("Auxiliary collection will be unsharded");
        }

        const foreignBulk = coll_aux.initializeUnorderedBulkOp();
        for (let i = 0; i < this.numDocs; ++i) {
            foreignBulk.insert({_id: i, to: i});
        }

        res = foreignBulk.execute();
        assert.commandWorked(res);
        assert.eq(this.numDocs, res.nInserted);
        assert.eq(this.numDocs, coll_aux.find().itcount());

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
                assert.commandWorked(coll_aux.createIndex({to: 1}));
                jsTestLog("Using index join");
                this.allowDiskUse = false;
            } else {
                jsTestLog("Using nested loop join");
                this.allowDiskUse = false;
            }
        }
    }

    function teardown(db, collName, cluster) {
        // Drop indexes, if any were created.
        assert.commandWorked(db[collName].dropIndexes());
        db[getAuxiliaryCollection(collName)].drop();

        // TODO SERVER-89663: We restore the original transaction lifetime limit since there may be
        // concurrent executions that relied on the old value.
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({
                setParameter: 1,
                transactionLifetimeLimitSeconds:
                    this.originalTransactionLifetimeLimitSeconds[db.getMongo().host]
            }));
        });
    }

    return {
        threadCount: 10,
        iterations: 400,
        states: states,
        transitions: transitions,
        data: data,
        setup: setup,
        teardown: teardown,
    };
})();
