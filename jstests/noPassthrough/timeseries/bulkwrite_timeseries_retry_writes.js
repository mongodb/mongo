/**
 * Tests retrying of time-series insert operations.
 *
 * This runs timeseries_retry_writes.js while overriding all CRUD commands to use bulkWrite.
 *
 * @tags: [
 *   featureFlagBulkWriteCommand,
 *   requires_replication,
 * ]
 */
await import('jstests/libs/override_methods/single_crud_op_as_bulk_write.js');
await import('jstests/noPassthrough/timeseries/timeseries_retry_writes.js');
