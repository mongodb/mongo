/**
 * Tests that setFCV rejects upgrades to FCV 9.0 or higher, both in dry-run mode and during the
 * actual upgrade, while a compound wildcard index with an invalid wildcardProjection exists.
 * Dropping the invalid index must unblock the upgrade, and upgrades to an FCV below 9.0 must not
 * be blocked.
 *
 * Such indexes can no longer be created (SERVER-113685), so the test bypasses the createIndexes
 * validation with a failpoint to stand in for an index created by an older binary.
 *
 * TODO (SERVER-132386): Remove this file once 10.0 becomes last LTS.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    checkFCV,
    lastContinuousFCV,
    lastLTSFCV,
    latestFCV,
} from "src/mongo/shell/feature_compatibility_version.js";

describe("FCV upgrade with valid wildcard indexes", function () {
    let conn;
    let adminDB;

    before(function () {
        conn = MongoRunner.runMongod({});
        adminDB = conn.getDB("admin");

        // Downgrade FCV to test the upgrade path after.
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        checkFCV(adminDB, lastLTSFCV);

        const coll = conn.getDB(jsTestName())["validWildcardIndexColl"];
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorked(coll.createIndex({"$**": 1}, {name: "validWildcardIndex"}));
        assert.commandWorked(
            coll.createIndex(
                {"a": 1, "$**": 1},
                {name: "validCompoundWildcardIndex", wildcardProjection: {b: 1}},
            ),
        );
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("is not blocked", function () {
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        checkFCV(adminDB, latestFCV);
    });
});

describe("FCV upgrade with an invalid compound wildcard index", function () {
    const indexName = "invalidWildcardIndex";
    let conn;
    let adminDB;
    let coll;

    before(function () {
        conn = MongoRunner.runMongod({});
        adminDB = conn.getDB("admin");
        coll = conn.getDB(jsTestName())["invalidWildcardIndexColl"];

        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV, confirm: true}),
        );
        checkFCV(adminDB, lastLTSFCV);

        assert.commandWorked(coll.insert({a: 1}));
        const fp = configureFailPoint(conn, "skipWildcardIndexProjectionValidation");
        assert.commandWorked(
            coll.createIndex({"a": 1, "$**": 1}, {name: indexName, wildcardProjection: {_id: 0}}),
        );
        fp.off();
    });

    after(function () {
        MongoRunner.stopMongod(conn);
    });

    it("fails the setFCV dry-run", function () {
        const res = adminDB.runCommand({
            setFeatureCompatibilityVersion: latestFCV,
            dryRun: true,
        });
        assert.commandFailedWithCode(res, ErrorCodes.CannotUpgrade);
        assert(
            res.errmsg.includes(
                `compound wildcard index '${indexName}' with an invalid wildcardProjection`,
            ),
            "unexpected dry-run error message",
            {res},
        );
    });

    it("fails the actual upgrade and leaves the FCV unchanged", function () {
        assert.commandFailedWithCode(
            adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            ErrorCodes.CannotUpgrade,
        );
        checkFCV(adminDB, lastLTSFCV);
    });

    it("does not block upgrades below the 9.0 boundary", function () {
        // Once 9.0 is the lowest supported FCV, upgrades below the boundary no longer exist (and
        // any remaining upgrade is rightly blocked), so there is nothing left to verify here.
        if (MongoRunner.compareBinVersions(lastContinuousFCV, "9.0") >= 0) {
            return;
        }

        // Upgrading from lastLTS to lastContinuous is only allowed as an internal operation, so
        // the request has to pass 'fromConfigServer'.
        assert.commandWorked(
            adminDB.runCommand({
                setFeatureCompatibilityVersion: lastContinuousFCV,
                confirm: true,
                fromConfigServer: true,
            }),
        );
        checkFCV(adminDB, lastContinuousFCV);

        // Upgrading to 9.0 or higher still fails from the intermediate FCV.
        assert.commandFailedWithCode(
            adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
            ErrorCodes.CannotUpgrade,
        );
        checkFCV(adminDB, lastContinuousFCV);
    });

    it("succeeds once the invalid index is dropped", function () {
        assert.commandWorked(coll.dropIndex(indexName));
        assert.commandWorked(
            adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        );
        checkFCV(adminDB, latestFCV);
    });
});
