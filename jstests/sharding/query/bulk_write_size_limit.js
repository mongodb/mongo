/**
 * Tests that the maximum size limit for a bulk write response is calculated correctly and does not
 * consider staleness errors that will be retried towards the final count.
 * @tags: [
 *   multiversion_incompatible,
 *   requires_fcv_80,
 *    # TODO (SERVER-97257): Re-enable this test or add an explanation why it is incompatible.
 *    embedded_router_incompatible,
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";
const st = new ShardingTest({
    shards: 2,
    mongos: 2,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {setParameter: {logComponentVerbosity: tojson({sharding: 4})}}
});

// Performing an insert through bulkWrite on a sharded collection will cause a
// CannotImplicitlyCreateCollectionError if the collection does not exist. This will be resolved by
// mongos and the write will be retried. For an unordered bulkWrite this original child batch can
// cause a lot of error messages to be generated which used to trigger our max cursor size limit,
// even though these internal errors should not be counted since they will be retried by mongos.
// This test ensures that we do not trigger that condition here and succeed on all of the writes as
// expected.

const db_s0 = st.s0.getDB("test");
let opsArr = [];

for (let i = 0; i < 100000; i++) {
    opsArr.push({insert: 0, document: {a: i}});
}

assert.commandWorked(db_s0.adminCommand({
    bulkWrite: 1,
    ops: opsArr,
    ordered: false,
    nsInfo: [{ns: "test.repro"}],
    writeConcern: {w: 0}
}));

// Make sure that all documents were written to the collection.
assert.eq(100000, db_s0.repro.find().itcount());

st.stop();
