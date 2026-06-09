/**
 * Tests concurrent time-series inserts, with enough batches and data to force buckets to be closed
 * due to the memory usage threshold.
 *
 * @tags: [
 *   requires_timeseries,
 *   # Timeseries do not support multi-document transactions with inserts.
 *   does_not_support_transactions,
 * ]
 */

import {setParameterOnAllNodes} from "jstests/concurrency/fsm_workload_helpers/set_parameter.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

export const $config = (function () {
    const timeFieldName = "time";
    const metaFieldName = "tag";
    const numDocs = 100;

    function getCollectionName(collName) {
        return jsTestName() + "_" + collName;
    }

    const insert = function (db, collName, tid, ordered) {
        const docs = [];
        for (let i = 0; i < numDocs; ++i) {
            docs.push({
                [timeFieldName]: ISODate(),
                [metaFieldName]: tid * numDocs + i,
            });
        }
        TimeseriesTest.assertInsertWorked(db.runCommand({insert: collName, documents: docs, ordered: ordered}));
    };

    const states = {
        init: function (db, collName) {},

        insertOrdered: function (db, collNameSuffix) {
            let collName = getCollectionName(collNameSuffix);
            insert(db, collName, this.tid, true);
        },

        insertUnordered: function (db, collNameSuffix) {
            let collName = getCollectionName(collNameSuffix);
            insert(db, collName, this.tid, false);
        },
    };

    let originalIdleBucketExpiryThreshold;

    const setup = function (db, collNameSuffix, cluster) {
        let collName = getCollectionName(collNameSuffix);
        originalIdleBucketExpiryThreshold = setParameterOnAllNodes({
            cluster: cluster,
            paramName: "timeseriesIdleBucketExpiryMemoryUsageThreshold",
            newValue: 1024,
            onMongos: false,
        });

        assert.commandWorked(
            db.createCollection(collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
        );
    };

    const teardown = function (db, collNameSuffix, cluster) {
        let collName = getCollectionName(collNameSuffix);
        const bucketCatalog = db.serverStatus().bucketCatalog;
        if (bucketCatalog !== undefined) {
            jsTestLog(bucketCatalog);
        }
        jsTestLog(db.runCommand({collStats: collName}).timeseries);

        setParameterOnAllNodes({
            cluster: cluster,
            paramName: "timeseriesIdleBucketExpiryMemoryUsageThreshold",
            newValue: originalIdleBucketExpiryThreshold,
            onMongos: false,
        });
    };

    const standardTransition = {
        insertOrdered: 0.5,
        insertUnordered: 0.5,
    };

    const transitions = {
        init: standardTransition,
        insertOrdered: standardTransition,
        insertUnordered: standardTransition,
    };

    return {
        threadCount: 10,
        iterations: 250,
        setup: setup,
        teardown: teardown,
        states: states,
        transitions: transitions,
    };
})();
