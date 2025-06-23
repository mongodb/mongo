// Tests setting query settings `reject` flag fails only the relevant changeStream query.
// @tags: [
//   requires_fcv_81,
//   assumes_read_preference_unchanged,
//   assumes_read_concern_unchanged,
//   change_stream_does_not_expect_txns,
//   uses_change_streams,
//   directly_against_shardsvrs_incompatible,
//   simulate_atlas_proxy_incompatible,
//   does_not_support_stepdowns,
//   # Modifies the pipeline so PQS won't match
//   do_not_run_in_whole_cluster_passthrough,
//   do_not_run_in_whole_db_passthrough,
// ]
//

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const coll = assertDropAndRecreateCollection(db, jsTestName());
const qsutils = new QuerySettingsUtils(db, coll.getName());

// Test with collection-specific and collection-less (i.e. database-specific) change streams.
[false, true].forEach((collectionLess) => {
    qsutils.removeAllQuerySettings();
    qsutils.assertRejection({
        query: qsutils.makeAggregateQueryInstance(
            {pipeline: [{$changeStream: {}}, {$match: {operationType: "insert"}}]}, collectionLess),
        queryPrime: qsutils.makeAggregateQueryInstance(
            {pipeline: [{$changeStream: {}}, {$match: {operationType: "delete"}}]}, collectionLess),
        unrelatedQuery: qsutils.makeAggregateQueryInstance(
            {pipeline: [{$changeStream: {}}, {$match: {"fullDocument.string": "value"}}]},
            collectionLess),
    });
});

// Test various change stream pipeline-related falgs.
["showSystemEvents", "showExpandedEvents"].forEach((flag) => {
    qsutils.removeAllQuerySettings();

    // Test that a change stream pipeline with the flag set to true has a different shape than a
    // pipeline with the flag set to false.
    qsutils.assertRejection({
        query: qsutils.makeAggregateQueryInstance({pipeline: [{$changeStream: {[flag]: true}}]}),
        queryPrime:
            qsutils.makeAggregateQueryInstance({pipeline: [{$changeStream: {[flag]: true}}]}),
        unrelatedQuery:
            qsutils.makeAggregateQueryInstance({pipeline: [{$changeStream: {[flag]: false}}]}),
    });

    // Test that a change stream pipeline with the flag set to false has a different shape than a
    // pipeline without the flag set at all.
    qsutils.removeAllQuerySettings();
    qsutils.assertRejection({
        query: qsutils.makeAggregateQueryInstance({pipeline: [{$changeStream: {[flag]: false}}]}),
        queryPrime:
            qsutils.makeAggregateQueryInstance({pipeline: [{$changeStream: {[flag]: false}}]}),
        unrelatedQuery: qsutils.makeAggregateQueryInstance({pipeline: [{$changeStream: {}}]}),
    });
});
