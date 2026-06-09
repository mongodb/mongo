/**
 * Verifies that the compact command runs successfully on replica set secondaries.
 *
 * @tags: [
 *     requires_fcv_90,
 *     requires_compact,
 *     requires_persistence,
 *     requires_replication,
 *     requires_wiredtiger,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("compact on replica set members", function () {
    let rst, primary, secondary;

    const dbName = "test";
    const collName = "compact_secondary";

    before(function () {
        rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        secondary = rst.getSecondary();
        secondary.setSecondaryOk();
        const docs = Array.from({length: 100}, (_, i) => ({_id: i, x: "a".repeat(1000)}));
        assert.commandWorked(primary.getDB(dbName)[collName].insertMany(docs));
        rst.awaitReplication();
    });

    after(function () {
        rst.stopSet();
    });

    it("succeeds on a secondary", function () {
        assert.commandWorked(secondary.getDB(dbName).runCommand({compact: collName}));
    });

    it("fails on a primary without force:true", function () {
        assert.commandFailedWithCode(
            primary.getDB(dbName).runCommand({compact: collName}),
            ErrorCodes.IllegalOperation,
        );
    });

    it("succeeds on a primary with force:true", function () {
        assert.commandWorked(primary.getDB(dbName).runCommand({compact: collName, force: true}));
    });
});
