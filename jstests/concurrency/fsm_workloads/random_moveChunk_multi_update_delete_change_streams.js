'use strict';

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
load('jstests/concurrency/fsm_libs/extend_workload.js');
load('jstests/concurrency/fsm_workloads/random_moveChunk_base.js');

var $config = extendWorkload($config, function($config, $super) {
    $config.threadCount = 5;
    $config.iterations = 50;

    // Number of documents per partition. Note that there is one chunk per partition and one
    // partition per thread.
    $config.data.partitionSize = 100;

    // Queue of pending change stream events regarding this tid's documents expected to be seen.
    // Only used when running in transaction suites (due to SERVER-20361, because
    // multi-updates/deletes may be applied 0, 1 or more times, thus we cannot foresee the exact
    // number of change stream events that will be produced for each multi write).
    $config.data.pendingEvents = [];

    // Lists that store the update/delete change stream events seen by this tid. Used to check that
    // no duplicate events (e.g due to writes on orphans) are received. Only used in non-transaction
    // suites, since for transaction suites 'pendingEvents' already tests the uniqueness property.
    $config.data.seenUpdates = [];
    $config.data.seenDeletes = [];

    const withSkipRetryOnNetworkError = (fn) => {
        const previousSkipRetryOnNetworkError = TestData.skipRetryOnNetworkError;
        TestData.skipRetryOnNetworkError = true;

        let res = undefined;
        try {
            res = fn();
        } catch (e) {
            throw e;
        } finally {
            TestData.skipRetryOnNetworkError = previousSkipRetryOnNetworkError;
        }

        return res;
    };

    const runWithManualRetriesIfInStepdownSuite = (fn) => {
        if (TestData.runningWithShardStepdowns) {
            var result = undefined;
            assert.soonNoExcept(() => {
                result = withSkipRetryOnNetworkError(fn);
                return true;
            });
            return result;
        } else {
            return fn();
        }
    };

    $config.states.multiUpdate = function(db, collName, connCache) {
        const collection = db[collName];
        const id = this.getIdForThread(collName);

        const doMultiUpdate = () => {
            const result = db.runCommand(
                {update: collName, updates: [{q: {x: id}, u: {$inc: {counter: 1}}, multi: true}]});
            assertWhenOwnColl.commandWorked(result);
            jsTest.log("tid:" + this.tid + " multiUpdate _id: " + id +
                       " at operationTime: " + tojson(result.operationTime));
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
        const expectedDocsIndex = this.expectedDocs.findIndex(item => item._id === id);
        if (expectedDocsIndex != -1) {
            this.pendingEvents.push({
                operationType: 'update',
                _id: id,
                counter: this.expectedDocs[expectedDocsIndex].counter + 1
            });

            // Update the in-memory representation of the updated document.
            this.expectedDocs[expectedDocsIndex].counter++;
        }
    };

    $config.states.multiDelete = function(db, collName, connCache) {
        const collection = db[collName];
        const id = this.getIdForThread(collName);

        const doMultiDelete = () => {
            const result = db.runCommand(
                {delete: collName, deletes: [{q: {x: id}, limit: 0 /* multi:true */}]});
            assertWhenOwnColl.commandWorked(result);

            jsTest.log("tid:" + this.tid + " multiDelete _id: " + id +
                       " at operationTime: " + tojson(result.operationTime));
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
        if (this.expectedDocs.some(item => item._id === id)) {
            this.pendingEvents.push({operationType: 'delete', _id: id});
        }

        // Remove the in-memory representation of the deleted document.
        this.expectedDocs = this.expectedDocs.filter(item => item._id !== id);
    };

    $config.states.init = function init(db, collName, connCache) {
        // Keep only this tid's documents
        this.expectedDocs = this.expectedDocs.filter(item => item.tid === this.tid);

        if (TestData.runningWithShardStepdowns) {
            fsm.forceRunningOutsideTransaction(this);
            runWithManualRetriesIfInStepdownSuite(() => {
                $super.states.init.apply(this, arguments);
            });
        } else {
            $super.states.init.apply(this, arguments);
        }
    };

    $config.states.checkChangeStream = function init(db, collName, connCache) {
        // Cannot establish changeStream in a txn.
        fsm.forceRunningOutsideTransaction(this);

        const establishChangeStreamCursor = () => {
            const csOptions = this.resumeToken ? {resumeAfter: this.resumeToken} : {
                startAtOperationTime:
                    Timestamp(this.startAtOperationTime.t, this.startAtOperationTime.i)
            };

            return db[collName].watch([], csOptions);
        };

        const consumeChangeStream = () => {
            let changeStream = establishChangeStreamCursor();

            let events = [];
            while (changeStream.hasNext()) {
                events.push(changeStream.next());
            }

            changeStream.close();
            return events;
        };

        const waitForChangeStreamEvent = () => {
            runWithManualRetriesIfInStepdownSuite(() => {
                let changeStream = establishChangeStreamCursor();
                assert.soon(() => changeStream.hasNext());
                changeStream.close();
                return true;
            });
        };

        while (true) {
            const events = runWithManualRetriesIfInStepdownSuite(consumeChangeStream);
            if (events.length > 0) {
                const lastEvent = events[events.length - 1];
                this.resumeToken = lastEvent._id;
                jsTest.log("tid:" + this.tid +
                           " timestamp of the last event:" + tojson(lastEvent.clusterTime));
            }

            events.forEach(event => {
                // Only process events related to this tid's documents.
                if (this.ownedIds[collName].includes(event.documentKey._id)) {
                    if (TestData.runInsideTransaction) {
                        assertAlways(
                            this.pendingEvents.length > 0,
                            "Did not expect to see any change stream event regarding this tid's documents. Event: " +
                                tojson(event));

                        jsTest.log("tid:" + this.tid + " changeStream event: operationType: " +
                                   event.operationType + "; _id: " + event.documentKey._id);

                        // Check that the received event corresponds to the next expected event.
                        const nextPendingEvent = this.pendingEvents.shift();
                        assertAlways.eq(nextPendingEvent.operationType, event.operationType);
                        assertAlways.eq(nextPendingEvent._id, event.documentKey._id);

                        if (event.operationType === 'update') {
                            assert.eq(nextPendingEvent.counter,
                                      event.updateDescription.updatedFields.counter);
                        }
                    } else {
                        // Check that no duplicate events are seen on the change stream.
                        // - For deletes this means that we should not see the same document deleted
                        // more than once (since the workload does not perform any inserts after
                        // setup).
                        // - For updates, this means that for each document, we should never see the
                        // same updated value more than once. This is because the updates are {$inc:
                        // 1}, so they must be strictly incrementing.
                        if (event.operationType === 'delete') {
                            assertAlways(!this.seenDeletes.includes(event.documentKey._id),
                                         "Found duplicate change stream event for delete on _id: " +
                                             event.documentKey._id);
                            this.seenDeletes.push(event.documentKey._id);
                        } else if (event.operationType === 'update') {
                            const idAndUpdate = {
                                _id: event.documentKey._id,
                                updatedFields: event.updateDescription.updatedFields
                            };

                            assert(!this.seenUpdates.some(
                                       item => item._id === idAndUpdate._id &&
                                           bsonWoCompare(item.updatedFields,
                                                         idAndUpdate.updatedFields) == 0),
                                   "Found duplicate change stream event for update on _id: " +
                                       event.documentKey._id + ", update: " +
                                       tojson(event.updateDescription.updatedFields));
                            this.seenUpdates.push(idAndUpdate);
                        }
                    }
                }
            });

            // If running in a txn suite, ensure we eventually see all pending events. We can only
            // do this in txn suites because on non-txn suites we have no guarantee that a
            // multi-update/delete actually was actually applied on all the intended documents
            // (SERVER-20361).
            if (TestData.runInsideTransaction && this.pendingEvents.length > 0) {
                jsTest.log("tid:" + this.tid +
                           " waiting for more change stream events. Next expected event: " +
                           tojson(this.pendingEvents[0]));
                waitForChangeStreamEvent();
                jsTest.log("tid:" + this.tid +
                           " now there should be more change stream events available");
                continue;
            }

            break;
        }
    };

    let previousWritePeriodicNoops;

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        // Set 'writePeriodicNoops' to ensure liveness on change stream events in the case where one
        // of the shards has no changes to report.
        cluster.executeOnMongodNodes((db) => {
            const res = db.adminCommand({setParameter: 1, writePeriodicNoops: true});
            assert.commandWorked(res);
            previousWritePeriodicNoops = res.was;
        });

        // Set the 'x' field to mirror the '_id' and 'skey' fields. 'x' will be used as query for
        // {multi: true} writes.
        db[collName].find({}).forEach(doc => {
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
    };

    $config.teardown = function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(
                db.adminCommand({setParameter: 1, writePeriodicNoops: previousWritePeriodicNoops}));
        });

        $super.teardown.apply(this, arguments);
    };

    $config.transitions = {
        init: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4},
        moveChunk: {moveChunk: 0.2, multiUpdate: 0.3, multiDelete: 0.3, checkChangeStream: 0.2},
        multiUpdate: {moveChunk: 0.2, multiUpdate: 0.3, multiDelete: 0.3, checkChangeStream: 0.2},
        multiDelete: {moveChunk: 0.2, multiUpdate: 0.3, multiDelete: 0.3, checkChangeStream: 0.2},
        checkChangeStream: {moveChunk: 0.2, multiUpdate: 0.4, multiDelete: 0.4}
    };

    return $config;
});
