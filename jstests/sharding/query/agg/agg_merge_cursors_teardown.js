/**
 * Verifies mongos does not crash when a sharded cursor is exhausted exactly at batch boundary.
 *
 * @tags: [
 *  requires_sharding,
 *  requires_fcv_90,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("sharded merge-cursor teardown does not crash mongos", function () {
    let st;
    let coll;
    const batchSize = 10;

    before(function () {
        st = new ShardingTest({shards: 2});
        const testDb = st.s.getDB(jsTestName());
        coll = testDb[jsTestName()];
        coll.drop();

        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
        );

        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < 100; i++) {
            bulk.insert({_id: i, val: i});
        }
        assert.commandWorked(bulk.execute());
    });

    after(function () {
        st.stop();
    });

    it("results > batchSize with merge-side $group", function () {
        const count = coll
            .aggregate([{$group: {_id: "$val", s: {$sum: 1}}}], {cursor: {batchSize}})
            .itcount();
        assert.eq(count, 100);
    });

    it("results == batchSize with merge-side $group (crash alignment)", function () {
        const count = coll
            .aggregate([{$group: {_id: "$val", s: {$sum: 1}}}, {$limit: batchSize}], {
                cursor: {batchSize},
            })
            .itcount();
        assert.eq(count, batchSize);
    });
});
