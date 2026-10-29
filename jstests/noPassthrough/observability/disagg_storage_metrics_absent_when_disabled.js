/**
 * Verifies that the disaggregated-storage (DSC) serverStatus metric tree is entirely absent on a
 * normal node where disaggregated storage is disabled (an ASC node).
 *
 * NB: This is a test of absence. If `metrics.dissaggStorage` is renamed or
 * removed, this test will pass vacuously. It must be updated along with changes to that metric.
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("disaggStorage serverStatus metrics on an ASC node", function () {
    before(function () {
        // A plain standalone mongod leaves disaggregatedStorageEnabled at its default of false.
        this.conn = MongoRunner.runMongod({});
        assert.neq(null, this.conn, "mongod failed to start");
        this.adminDB = this.conn.getDB("admin");
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("does not expose the metrics.disaggStorage subtree", function () {
        const status = assert.commandWorked(this.adminDB.serverStatus());
        assert(status.hasOwnProperty("metrics"), "serverStatus is missing metrics", {status});
        assert(
            !status.metrics.hasOwnProperty("disaggStorage"),
            "metrics.disaggStorage must be absent when disaggregatedStorageEnabled=false",
            {metrics: status.metrics},
        );
    });
});
