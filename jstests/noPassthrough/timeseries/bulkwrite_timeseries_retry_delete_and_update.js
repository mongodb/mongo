/**
 * Tests retrying of time-series delete and update operations that are eligible for retryable writes
 * (specifically single deletes and updates).
 *
 * This runs timeseries_retry_delete_and_update.js while overriding all CRUD commands to use
 * bulkWrite.
 *
 * @tags: [
 *   requires_replication,
 *   requires_timeseries,
 *   requires_fcv_80,
 *   featureFlagTimeseriesUpdatesSupport,
 *   featureFlagTrackUnshardedCollectionsUponCreation,
 * ]
 */
await import('jstests/libs/override_methods/single_crud_op_as_bulk_write.js');
await import('jstests/noPassthrough/timeseries/timeseries_retry_delete_and_update.js');
