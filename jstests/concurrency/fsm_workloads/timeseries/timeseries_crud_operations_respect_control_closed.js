/**
 * Extends timeseries_insert_idle_bucket_expiration.js with concurrent findAndModify operations that
 * set a bucket's control.closed field to true.
 *
 * This test tests that concurrent user inserts, updates, and deletes on open buckets and concurrent
 * reopening requests on buckets respect the control.closed:true field and do not write to closed
 * buckets.
 *
 * @tags: [
 *  requires_timeseries,
 *  requires_non_retryable_writes,
 *  # Timeseries do not support multi-document transactions with inserts.
 *   does_not_support_transactions,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {
    $config as $baseConfig
} from 'jstests/concurrency/fsm_workloads/timeseries/timeseries_insert_idle_bucket_expiration.js';
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    const data = {
        timeFieldName: 'time',
        metaFieldName: 'tag',
        numDocs: 1,
        numBucketMetaFieldsPerThread: 100,
        bucketMetaFieldName: "meta",
        numBucketsToCloseAtATime: 5,
        // This is the name of the collection we will use to store buckets that we have set
        // the control.closed field to true for. At the end of this test, we will go
        // through them and make sure that they have not been written to after this.
        bucketValidationCollName: "timeseries_crud_operations_respect_control_closed_log",
    };

    $config.states.init = function(db, collNameSuffix) {
        // Reading at which this thread should start inserting. The starting point begins after
        // the seed data and is based on the thread id to ensure uniqueness across inserted
        // values.
        this.readingNo = data.numDocs;
    };

    function getCollectionName(collName) {
        return jsTestName() + "_" + collName;
    }

    /**
     * TODO (SERVER-88275): Revisit this, see whether we still want to accept the QueryPlanKilled
     * error.
     */
    const find = function(db, collName, queryFilter) {
        let documents;
        assert.soon(() => {
            try {
                documents = db[collName].find(queryFilter).toArray();
                return true;
            } catch (e) {
                if (e.code === ErrorCodes.QueryPlanKilled) {
                    // Retry. Can happen due to concurrent move collection.
                    return false;
                }
                throw e;
            }
        });
        return documents;
    };

    const insert = function(db,
                            collName,
                            tid,
                            ordered,
                            newReadingNo,
                            numBucketsToInsertInto,
                            numDocumentsToInsertPerBucket) {
        let docs = [];
        // Insert numBucketsDocumentsToInsertPerBucket into numBucketsToInsertInto distinct buckets.
        for (let i = 0; i < numBucketsToInsertInto; ++i) {
            for (let j = 0; j < numDocumentsToInsertPerBucket; ++j) {
                docs.push({
                    [data.timeFieldName]: ISODate(),
                    [data.metaFieldName]: (tid * data.numDocs) + i,
                    readingNo: newReadingNo + j,
                });
            }
        }

        // Shuffle our data.
        docs = Array.shuffle(docs);
        const additionalCodesToRetry = [ErrorCodes.NoProgressMade];

        retryOnRetryableError(() => {
            db.runCommand({insert: collName, documents: docs, ordered: ordered});
        }, 100, undefined, additionalCodesToRetry);
    };

    $config.states.setControlClosedTrue = function setControlClosedTrue(db, collNameSuffix) {
        const collName = getCollectionName(collNameSuffix);
        const bucketsCollName = "system.buckets." + collName;
        for (let i = 0; i < data.numBucketsToCloseAtATime; i++) {
            const bucketMeta =
                Random.randInt($baseConfig.threadCount * data.numBucketMetaFieldsPerThread);
            // Get a random bucket metadata, and then find a bucket with that metadata that does not
            // have its control.closed field set to true, and that is still being inserted into (its
            // count is less than the max amount of documents per bucket).
            const res = assert.commandWorked(db.runCommand({
                findAndModify: bucketsCollName,
                query: {
                    [data.bucketMetaFieldName]: bucketMeta,
                    "control.closed": {$exists: false},
                },
                sort: {"meta": 1, "control.max.timeField": -1},
                new: true,
                update: {$set: {"control.closed": true}}
            }));
            if (res.value) {
                assert.commandWorked(
                    db.runCommand({insert: data.bucketValidationCollName, documents: [res.value]}));
            }
        }
    };

    $config.states.insertOrdered = function(db, collNameSuffix) {
        const collName = getCollectionName(collNameSuffix);
        insert(db,
               collName,
               this.tid,
               true,
               this.readingNo,
               data.numBucketMetaFieldsPerThread,
               data.numDocs);
        this.readingNo += data.numDocs;
    };

    $config.states.insertUnordered = function(db, collNameSuffix) {
        const collName = getCollectionName(collNameSuffix);
        insert(db,
               collName,
               this.tid,
               false,
               this.readingNo,
               data.numBucketMetaFieldsPerThread,
               data.numDocs);
        this.readingNo += data.numDocs;
    };

    $config.states.deleteMany = function deleteMany(db, collNameSuffix) {
        const collName = getCollectionName(collNameSuffix);
        // Delete a readingNo in the range of [0, this.readingNo]. While it's possible and likely
        // that the bucket we delete from is not actually one that this particular thread has
        // written to, by capping the upper limit at this thread's readingNo this should ensure
        // that the delete operations finds at least one document to delete.
        const readingNo = Random.randInt(this.readingNo);
        assert.commandWorked(db[collName].deleteMany({readingNo: readingNo}));
    };

    $config.states.deleteOne = function deleteOne(db, collNameSuffix) {
        const collName = getCollectionName(collNameSuffix);
        const readingNo = Random.randInt(this.readingNo);
        assert.commandWorked(db[collName].deleteOne({readingNo: readingNo}));
    };

    // TODO SERVER-93150: Add states with concurrent arbitrary user updates.

    $config.setup = function(db, collNameSuffix, cluster) {
        $super.setup.apply(this, [db, collNameSuffix, cluster]);
        // Create a bucket for each metadata and insert some measurements into it. This prevents
        // setControlClosedTrue from trying to modify buckets that haven't been created yet.
        const docs = [];
        const collName = getCollectionName(collNameSuffix);
        // Build an index so that, when we find arbitrary buckets to close, we can efficiently find
        // the most recent bucket for the series.
        assert.commandWorked(db[collName].createIndex({"tag": 1, "time": -1}));
        const numSensors = data.numBucketMetaFieldsPerThread * $baseConfig.threadCount;
        for (let i = 0; i < numSensors; i++) {
            for (let j = 0; j < data.numDocs; j++) {
                docs.push({[data.timeFieldName]: ISODate(), [data.metaFieldName]: i, readingNo: j});
            }
        }
        TimeseriesTest.assertInsertWorked(db.runCommand({insert: collName, documents: docs}));
        jsTestLog(db.runCommand({collStats: collName}).timeseries);
    };

    $config.teardown = function(db, collNameSuffix, cluster) {
        $super.teardown.apply(this, [db, collNameSuffix, cluster]);

        const bucketsToValidate = find(db, data.bucketValidationCollName, {});
        const collName = getCollectionName(collNameSuffix);
        const bucketsCollName = "system.buckets." + collName;
        const numTotalBuckets = find(db, bucketsCollName, {}).length;
        // Let's go through all of the buckets that we had set the control.closed field to true for,
        // and validate that they have not been written to since.
        jsTestLog(`Validating ${
            bucketsToValidate
                .length} buckets that had their control.closed field set to true out of ${
            numTotalBuckets} total buckets`);
        let numDocsInBucketWhenClosed = 0;
        for (let i = 0; i < bucketsToValidate.length; i++) {
            const bucket = bucketsToValidate[i];
            numDocsInBucketWhenClosed += bucket.control.count;
            const bucketsInCollection = find(db, bucketsCollName, {_id: bucket._id});
            assert.eq(bucketsInCollection.length, 1);
            const bucketInCollection = bucketsInCollection[0];
            const errMsg = `Expected bucket ${tojson(bucket)} and actual bucket ${tojson(bucketInCollection)} did not match; bucket may have had writes to it after the control.closed field was set.`;
            assert.docEq(bucket, bucketInCollection, errMsg);
        }
    };

    const standardTransition = {
        insertOrdered: 1,
        insertUnordered: 1,
        setControlClosedTrue: 1,
        deleteMany: 1,
        deleteOne: 1,
    };

    $config.transitions = {
        init: standardTransition,
        insertOrdered: standardTransition,
        insertUnordered: standardTransition,
        setControlClosedTrue: standardTransition,
        deleteMany: standardTransition,
        deleteOne: standardTransition,
    };

    return $config;
});
