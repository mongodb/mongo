/**
 * Tests that serverStatus reports the two on-disk query-spilling storage size gauges: the spill
 * WiredTiger instance's storageSize and the temporary-file placeholder. Summing the two yields the
 * total local-disk spilling footprint.
 *
 * @tags: [requires_wiredtiger]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("query spilling storage size gauges in serverStatus", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({});
        this.admin = this.conn.getDB("admin");
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("reports the file-based spilling placeholder gauge", function () {
        const {query} = this.admin.serverStatus().metrics;
        assert(query.hasOwnProperty("spilling"), "missing metrics.query.spilling", {query});
        // TODO(SERVER-129889): Assert the gauge rises and falls with spilling once implemented.
        assert.eq(0, query.spilling.fileSpilledStorageSize, "expected placeholder 0", {
            spilling: query.spilling,
        });
    });

    it("reports the spill WiredTiger storage size gauge", function () {
        const spillWT = this.admin.serverStatus().spillWiredTiger;
        assert(spillWT, "missing spillWiredTiger section");
        assert(spillWT.hasOwnProperty("storageSize"), "missing spillWiredTiger.storageSize", {
            spillWT,
        });
        assert.gte(spillWT.storageSize, 0, "storageSize is not a non-negative number", {spillWT});
    });
});
