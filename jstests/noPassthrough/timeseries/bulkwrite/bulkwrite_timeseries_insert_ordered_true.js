/**
 * Tests that time-series inserts respect {ordered: true}.
 *
 * This runs timeseries_insert_ordered_true.js while overriding all CRUD commands to use bulkWrite.
 *
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

// TODO(SERVER-102050): re-enable if these tests are still relevant
quit();

await import('jstests/libs/override_methods/single_crud_op_as_bulk_write.js');
await import('jstests/noPassthrough/timeseries/write/timeseries_insert_ordered_true.js');
