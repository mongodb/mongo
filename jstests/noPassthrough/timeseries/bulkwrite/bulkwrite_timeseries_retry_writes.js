/**
 * Tests retrying of time-series insert operations.
 *
 * This runs timeseries_retry_writes.js while overriding all CRUD commands to use bulkWrite.
 *
 * @tags: [
 *   requires_fcv_80,
 *   requires_replication,
 * ]
 */
await import('jstests/libs/override_methods/single_crud_op_as_bulk_write.js');
await import('jstests/noPassthrough/timeseries/write/timeseries_retry_writes.js');
