/**
 * Tests that a last-lts binary refuses to start when the FCV document on disk contains a
 * targetVersion from the latest release (written by a latest binary that was interrupted
 * mid-upgrade), and that the latest binary can still start on the same data files.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {before, describe, it} from "jstests/libs/mochalite.js";

describe("resume interrupted FCV upgrade with old binary", function () {
    let dbpath = null;

    before(function () {
        // Start latest binary, downgrade to lastLTS, then hang the upgrade mid-way so that the
        // transitional doc {version:lastLTS, targetVersion:latest} is left on disk.
        const conn = MongoRunner.runMongod({binVersion: "latest"});
        assert.neq(null, conn);
        const adminDB = conn.getDB("admin");
        dbpath = conn.dbpath;

        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );

        const hangFP = configureFailPoint(conn, "hangWhileUpgrading");

        const awaitUpgrade = startParallelShell(
            `assert.commandWorked(db.adminCommand(
                {setFeatureCompatibilityVersion: "${latestFCV}", confirm: true}));`,
            conn.port,
        );

        hangFP.wait();

        MongoRunner.stopMongod(conn);
        awaitUpgrade({checkExitSuccess: false});
    });

    it("last-lts binary refuses to start when targetVersion is unknown to it", function () {
        assert.throws(
            () => MongoRunner.runMongod({dbpath, binVersion: "last-lts", noCleanData: true}),
            [],
            "expected last-lts mongod to fail when FCV doc has targetVersion: " + latestFCV,
        );
    });

    it("latest binary starts successfully on the transitional FCV doc", function () {
        const conn2 = MongoRunner.runMongod({dbpath, binVersion: "latest", noCleanData: true});
        try {
            assert.neq(null, conn2, "expected latest mongod to start on the transitional FCV doc");
            const fcvDoc = conn2
                .getDB("admin")
                .system.version.findOne({_id: "featureCompatibilityVersion"});
            assert.eq(fcvDoc.version, lastLTSFCV, "unexpected version field", {fcvDoc});
            assert.eq(fcvDoc.targetVersion, latestFCV, "unexpected targetVersion field", {fcvDoc});
        } finally {
            if (conn2) MongoRunner.stopMongod(conn2);
        }
    });
});
