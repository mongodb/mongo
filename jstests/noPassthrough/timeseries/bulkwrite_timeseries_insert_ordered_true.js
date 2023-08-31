/**
 * Tests that time-series inserts respect {ordered: true}.
 *
 * This runs timeseries_insert_ordered_true.js while overriding all CRUD commands to use bulkWrite.
 *
 * @tags: [
 *   featureFlagBulkWriteCommand,
 * ]
 */
await import('jstests/libs/override_methods/single_crud_op_as_bulk_write.js');
await import('jstests/noPassthrough/timeseries/timeseries_insert_ordered_true.js');
