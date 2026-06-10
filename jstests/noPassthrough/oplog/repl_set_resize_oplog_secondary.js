/**
 * Verifies that replSetResizeOplog runs successfully on a replica set secondary.
 *
 * @tags: [
 *   requires_replication,
 *   requires_wiredtiger,
 * ]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("replSetResizeOplog on replica set members", function () {
    const MB = 1024 * 1024;

    let rst, primary, secondary;

    before(function () {
        rst = new ReplSetTest({nodes: 2, oplogSize: 100});
        rst.startSet();
        rst.initiate();

        primary = rst.getPrimary();
        rst.awaitSecondaryNodes();
        secondary = rst.getSecondary();
        secondary.setSecondaryOk();
    });

    after(function () {
        rst.stopSet();
    });

    it("succeeds on a secondary", function () {
        assert.commandWorked(secondary.getDB("admin").runCommand({replSetResizeOplog: 1, size: 990}));
        assert.eq(secondary.getDB("local").oplog.rs.stats().maxSize, 990 * MB);
    });

    it("succeeds on a primary", function () {
        assert.commandWorked(primary.getDB("admin").runCommand({replSetResizeOplog: 1, size: 1000}));
        assert.eq(primary.getDB("local").oplog.rs.stats().maxSize, 1000 * MB);
    });
});
