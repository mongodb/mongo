/**
 * Tests that serverStatus reports the two on-disk query-spilling storage size gauges: the spill
 * WiredTiger instance's storageSize and the temporary-file gauge. Summing the two yields the total
 * local-disk spilling footprint.
 *
 * @tags: [requires_persistence, requires_wiredtiger]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";

describe("query spilling storage size gauges in serverStatus", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({});
        this.admin = this.conn.getDB("admin");
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    function fileSpilledStorageSize(admin) {
        return admin.serverStatus().metrics.query.spilling.fileSpilledStorageSize;
    }

    it("raises and lowers the file-based spilling gauge as a sort spills and finishes", function () {
        const dbName = "spill_file_gauge";
        const db = this.conn.getDB(dbName);
        const coll = db.spill_sort;
        coll.drop();

        // Force a classic, file-backed sort that spills every document to disk.
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceClassicEngine"}),
        );
        assert.commandWorked(
            db.adminCommand({setParameter: 1, internalQueryMaxBlockingSortMemoryUsageBytes: 1}),
        );

        const nDocs = 100;
        const pad = "x".repeat(1024);
        const bulk = coll.initializeUnorderedBulkOp();
        for (let i = 0; i < nDocs; i++) {
            bulk.insert({_id: i, a: nDocs - i, pad});
        }
        assert.commandWorked(bulk.execute());

        assert.eq(0, fileSpilledStorageSize(this.admin), "expected no spill files at rest");

        // Hang the collection scan feeding the sort partway through, so the spill file exists and is
        // partially written when we sample the gauge.
        const failPoint = configureFailPoint(db, "hangCollScanDoWork", {}, {skip: nDocs / 2});
        const awaitShell = startParallelShell(
            funWithArgs(
                (dbName, collName) => {
                    assert.eq(
                        db
                            .getSiblingDB(dbName)
                            [collName].aggregate([{$sort: {a: 1}}])
                            .itcount(),
                        100,
                    );
                },
                dbName,
                coll.getName(),
            ),
            this.conn.port,
        );

        failPoint.wait();
        assert.gt(
            fileSpilledStorageSize(this.admin),
            0,
            "expected the file spilling gauge to be positive mid-spill",
        );

        failPoint.off();
        awaitShell();

        // Once the sort completes, the spill file is destroyed and the gauge returns to zero. The
        // gauge is a Counter64, so serverStatus returns it as a NumberLong; use a coercing
        // comparison (the gauge is non-negative) rather than strict '=== 0'.
        assert.soon(
            () => fileSpilledStorageSize(this.admin) <= 0,
            () => `gauge did not return to 0, last value: ${fileSpilledStorageSize(this.admin)}`,
        );
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
