// Test an in memory sort memory assertion after a plan has "taken over" in the query optimizer
// cursor.
// @tags: [
//   # in memory variants won't treat this workload the same and may not fail.
//   requires_persistence,
//   requires_getmore,
// ]
//
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const collection = db.jstests_sortj;
collection.drop();

assert.commandWorked(collection.createIndex({a: 1}));

const numShards = FixtureHelpers.numberOfShardsForCollection(collection);

const big = new Array(100000).toString();
for (let i = 0; i < 1200 * numShards; ++i) {
    assert.commandWorked(collection.save({a: 1, b: big}));
    if (i % 5 == 0) {
        // TODO. SERVER-90366 remove this manual gc execution.
        //
        // This is to prevent this test from running out of memory when running in
        // bulk_write_multi_op_sharded_collections_jscore_passthrough suite. On that suite, inserts
        // are batched in groups of 5 and then flushed to an auxiliary cluster (aka bulk writes
        // cluster). Then the collections in both the normal and bulk write cluster are walked with
        // a cursor to ensure both have the exact same records. It is at that time when the system
        // runs out of memory so we prevent it by manually gc'ing after every such verification.
        gc();
    }
}

assert.throwsWithCode(
    () => collection.find({a: {$gte: 0}, c: null}).sort({d: 1}).allowDiskUse(false).itcount(),
    ErrorCodes.QueryExceededMemoryLimitNoDiskUseAllowed);
