/**
 * Tests that $collStats reports the WiredTiger table type as "file".
 *
 * @tags: [requires_wiredtiger, requires_persistence]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("$collStats WiredTiger type signal", function () {
    const dbName = "test";
    const collName = "storage_type_signal_asc";

    before(function () {
        this.testColl = db.getSiblingDB(dbName).getCollection(collName);
        // Assert against a collection created by this test (a plain replica set may not have
        // config.settings).
        assert.commandWorked(this.testColl.insert({x: 1}));
    });

    after(function () {
        this.testColl.drop();
    });

    it("reports WiredTiger type 'file'", function () {
        const res = this.testColl
            .aggregate([
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
