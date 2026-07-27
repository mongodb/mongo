/**
 * Tests that $collStats reports the WiredTiger table type as "file" when run through mongos.
 * Queries config.settings since it always exists and is readable through mongos.
 *
 * @tags: [requires_sharding, requires_wiredtiger, requires_persistence]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("$collStats WiredTiger type signal through mongos", function () {
    before(function () {
        this.st = new ShardingTest({shards: 1, mongos: 1});
    });

    after(function () {
        this.st.stop();
    });

    it("reports WiredTiger type 'file' through mongos", function () {
        const res = this.st.s
            .getDB("config")
            .settings.aggregate([
                {$collStats: {storageStats: {}}},
                {$project: {type: "$storageStats.wiredTiger.type"}},
            ])
            .toArray();

        assert.gt(res.length, 0, "expected at least one $collStats result", {res});
        for (const r of res) {
            assert.eq(r.type, "file", "Expected WiredTiger type 'file'", {res});
        }
    });
});
