/**
 * Performs a series of {multi: true} updates/deletes while moving chunks, and checks that the
 * expected change stream events are received and that no events are generated to writes on orphan
 * documents.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  uses_change_streams
 * ];
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {fsm} from "jstests/concurrency/fsm_libs/fsm.js";
import {
    runWithManualRetries,
    withSkipRetryOnNetworkError,
} from "jstests/concurrency/fsm_workload_helpers/stepdown_suite_helpers.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // Number of documents per partition. Note that there is one chunk per partition and one
    // partition per thread.
    $config.data.partitionSize = 100;

    // Keep track of the number of write operations. Used only as a sort element for the operations
    // side-table.
    $config.data.writesCount = 0;

    $config.states.multiUpdate = function (db, collName, connCache) {
        const id = this.getIdForThread(collName);

        const doMultiUpdate = () => {
            // moveChunk can kill the multiupdate command and return QueryPlanKilled, so retry in
            // that case.
            retryOnRetryableError(
                () => {
                    const result = db.runCommand({
                        update: collName,
                        updates: [{q: {x: id}, u: {$inc: {counter: 1}}, multi: true}],
                    });
                    assert.commandWorked(result);
                    jsTest.log(
                        "tid:" +
                            this.tid +
                            " multiUpdate _id: " +
                            id +
                            " at operationTime: " +
                            tojson(result.operationTime),
                    );
                },
                100,
                undefined,
                [ErrorCodes.QueryPlanKilled],
            );
        };

        if (TestData.runningWithShardStepdowns && !TestData.runInsideTransaction) {
            // network_error_and_txn_override refuses to run {multi: true} writes. Skip retries and
            // just ignore errors.
            try {
                withSkipRetryOnNetworkError(doMultiUpdate);
            } catch (e) {
                jsTest.log("Ignoring error: " + e.code);
            }
        } else {
            doMultiUpdate();
        }

        // If the document existed, we expect a change stream event to be eventually seen regarding
        // this update.
        const expectedDocsIndex = this.expectedDocs.findIndex((item) => item._id === id);
        if (expectedDocsIndex != -1) {
            // Log this operation on a side collection. It will be used later to validate the change
            // stream events.
            assert.commandWorked(
                db["operations"].insert({
                    tid: this.tid,
                    iteration: this.writesCount,
                    operationDetails: {
                        operationType: "update",
                        documentId: id,
                        counter: this.expectedDocs[expectedDocsIndex].counter + 1,
                    },
                }),
            );
            this.writesCount++;

            // Update the in-memory representation of the updated document.
            this.expectedDocs[expectedDocsIndex].counter++;
        }
    };

    $config.states.multiDelete = function (db, collName, connCache) {
        const id = this.getIdForThread(collName);

        const doMultiDelete = () => {
            const result = db.runCommand({delete: collName, deletes: [{q: {x: id}, limit: 0 /* multi:true */}]});
            assert.commandWorked(result);

            jsTest.log(
                "tid:" + this.tid + " multiDelete _id: " + id + " at operationTime: " + tojson(result.operationTime),
            );
        };

        if (TestData.runningWithShardStepdowns && !TestData.runInsideTransaction) {
            // network_error_and_txn_override refuses to run {multi: true} writes. Skip retries and
            // just ignore errors.
            try {
                withSkipRetryOnNetworkError(doMultiDelete);
            } catch (e) {
                jsTest.log("Ignoring error: " + e.code);
            }
        } else {
            doMultiDelete();
        }

        // If the document existed, we expect a change stream event to be eventually seen regarding
        // this delete.
        if (this.expectedDocs.some((item) => item._id === id)) {
            // Log this operation on a side collection. It will be used later to validate the change
            // stream events.
            assert.commandWorked(
                db["operations"].insert({
                    tid: this.tid,
                    iteration: this.writesCount,
                    operationDetails: {operationType: "delete", documentId: id},
                }),
            );
            this.writesCount++;
        }

        // Remove the in-memory representation of the deleted document.
        this.expectedDocs = this.expectedDocs.filter((item) => item._id !== id);
    };

    $config.states.init = function init(db, collName, connCache) {
        // Keep only this tid's documents
        this.expectedDocs = this.expectedDocs.filter((item) => item.tid === this.tid);

        if (TestData.runningWithShardStepdowns) {
            fsm.forceRunningOutsideTransaction(this);
            runWithManualRetries(() => {
                $super.states.init.apply(this, arguments);
            });
        } else {
            $super.states.init.apply(this, arguments);
        }
    };

    function checkChangeStream(db, collName, startAtOperationTime) {
        // Lists that store the update/delete change stream events seen by this tid. Used to check
        // that no duplicate events (e.g due to writes on orphans) are received. Only used in
        // non-transaction suites. For transaction suites we do a strict check on the expected
        // events.
        let seenUpdates = [];
        let seenDeletes = [];

        // Read the operations that the workload threads did.
        let operationsByTid = {}; // tid -> [operations]
        for (var tid = 0; tid < $config.threadCount; ++tid) {
            operationsByTid[tid] = db["operations"].find({tid: tid}).sort({iteration: 1}).toArray();
        }

        // Check change stream
        const changeStream = db[collName].watch([], {startAtOperationTime: startAtOperationTime});
        while (true) {
            assert.soon(() => changeStream.hasNext());
            const event = changeStream.next();

            if (event.operationType === "drop") {
                jsTest.log("tid: + " + tid + " checkChangeStream saw drop event");
                break;
            }

            jsTest.log(
                "changeStream event: operationType: " + event.operationType + "; _id: " + tojson(event.documentKey),
            );

            if (TestData.runInsideTransaction) {
                // Check that this event corresponds to the next outstanding operation one of the
                // worker threads did.
                let found = false;
                for (let tid = 0; tid < $config.threadCount; ++tid) {
                    const nextOperationForTid = operationsByTid[tid][0];
                    if (
                        nextOperationForTid &&
                        nextOperationForTid.operationDetails.operationType === event.operationType &&
                        nextOperationForTid.operationDetails.documentId === event.documentKey._id
                    ) {
                        found = true;

                        // Remove that operation from the array of outstanding operations.
                        operationsByTid[tid].shift();
                        break;
                    }
                }
                assert(
                    found,
                    "did not find worker thread operation matching the change stream event: " +
                        tojson(event) +
                        "; Outstanding operations: " +
                        tojson(operationsByTid),
                );
            } else {
                // Check that no duplicate events are seen on the change stream.
                // - For deletes this means that we should not see the same document deleted more
                // than once (since the workload does not perform any inserts after setup).
                // - For updates, this means that for each document, we should never see the same
                // updated value more than once. This is because the updates are {$inc: 1}, so they
                // must be strictly incrementing.
                if (event.operationType === "delete") {
                    assert(
                        !seenDeletes.includes(event.documentKey._id),
                        "Found duplicate change stream event for delete on _id: " + event.documentKey._id,
                    );
                    seenDeletes.push(event.documentKey._id);
                } else if (event.operationType === "update") {
                    const idAndUpdate = {
                        _id: event.documentKey._id,
                        updatedFields: event.updateDescription.updatedFields,
                    };

                    assert(
                        !seenUpdates.some(
                            (item) =>
                                item._id === idAndUpdate._id &&
                                bsonWoCompare(item.updatedFields, idAndUpdate.updatedFields) == 0,
                        ),
                        "Found duplicate change stream event for update on _id: " +
                            event.documentKey._id +
                            ", update: " +
                            tojson(event.updateDescription.updatedFields),
                    );
                    seenUpdates.push(idAndUpdate);
                }
            }
        }

        // If running in a txn suite, ensure we eventually see all events. We can only do this in
        // txn suites because on non-txn suites we have no guarantee that a multi-update/delete
        // actually was actually applied on all the intended documents (SERVER-20361).
        if (TestData.runInsideTransaction) {
            for (let tid = 0; tid < $config.threadCount; ++tid) {
                assert(
                    operationsByTid[tid].length === 0,
                    "Did not observe change stream event for all worker thread operations. Outstanding operations: " +
                        tojson(operationsByTid),
                );
            }
        }
    }

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        // Set the 'x' field to mirror the '_id' and 'skey' fields. 'x' will be used as query for
        // {multi: true} writes.
        db[collName].find({}).forEach((doc) => {
            db[collName].update({_id: doc._id}, {$set: {x: doc._id, counter: 0}});
        });

        // Store the operationTime after which we finished all updates to the collection during
        // setup. It will be used as a starting point for the changeStream.
        const operationTime = db.getSession().getOperationTime();
        this.startAtOperationTime = Timestamp(operationTime.t, operationTime.i);
        // Increment it so that the change stream won't see the last of the updates done as part of
        // setup()
        this.startAtOperationTime.i++;

        // Store an in-memory representation of the documents in the collection.
        this.expectedDocs = db[collName].find().toArray();

        // Make sure the 'operations' collection is created before we start running the FSM states.
        // This prevents failures in suites where states run in transactions, as creating a
        // collection within a multi-shard transaction is not allowed.
        db.createCollection("operations");
    };

    $config.teardown = function teardown(db, collName, cluster) {
        // Drop the collection as to have a sentinel event (drop) on the change stream.
        assert(db[collName].drop());

        // Validate the change stream events after setting 'writePeriodicNoops'  on all the nodes of
        // the cluster to ensure liveness in case there are nodes with no events to report.
        let previousWritePeriodicNoopsOnShards;
        let previousWritePeriodicNoopsOnConfigServer;

        cluster.executeOnMongodNodes((db) => {
            const res = db.adminCommand({setParameter: 1, writePeriodicNoops: true});
            assert.commandWorked(res);
            previousWritePeriodicNoopsOnShards = res.was;
        });
        cluster.executeOnConfigNodes((db) => {
            const res = db.adminCommand({setParameter: 1, writePeriodicNoops: true});
            assert.commandWorked(res);
            previousWritePeriodicNoopsOnConfigServer = res.was;
        });

        let startAtOperationTime = Timestamp(this.startAtOperationTime.t, this.startAtOperationTime.i);
        checkChangeStream(db, collName, startAtOperationTime);

        // Restore the original configuration.
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, writePeriodicNoops: previousWritePeriodicNoopsOnShards}),
            );
        });
        cluster.executeOnConfigNodes((db) => {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, writePeriodicNoops: previousWritePeriodicNoopsOnConfigServer}),
            );
        });

        $super.teardown.apply(this, arguments);
    };

    $config.transitions = {
        init: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
        moveChunk: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
        multiUpdate: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
        multiDelete: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
    };

    return $config;
});
