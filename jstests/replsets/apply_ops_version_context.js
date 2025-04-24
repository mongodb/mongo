/**
 * Tests that the `versionContext` oplog field, which allows applying an oplog entry using a
 * fixed FCV snapshot when checking feature flags, is allowed by the `applyOps` command.
 *
 * @tags: [
 *   requires_replication,
 *   # versionContext is only guaranteed to be available from fcv 8.2 onwards.
 *   requires_fcv_82,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
const db = rst.getPrimary().getDB("test");

const collName = "mycollection";
const cmdNss = db.getName() + ".$cmd";

const currentFCV =
    assert.commandWorked(db.adminCommand({getParameter: 1, featureCompatibilityVersion: 1}))
        .featureCompatibilityVersion.version;
const testVersionContext = {
    OFCV: currentFCV
};

// versionContext is allowed in applyOps, and gets replicated in the produced oplog entries
assert.commandWorked(db.adminCommand({
    applyOps: [{op: "c", ns: cmdNss, o: {create: collName}, versionContext: testVersionContext}]
}));

rst.awaitReplication();
assert(rst.getSecondary().getDB("local").oplog.rs.findOne({
    op: "c",
    ns: cmdNss,
    "o.create": collName,
    versionContext: testVersionContext,
}));

// versionContext is not allowed in nested applyOps
assert.commandFailedWithCode(db.adminCommand({
    applyOps: [{
        op: "c",
        ns: "admin.$cmd",
        o: {
            applyOps:
                [{op: "c", ns: cmdNss, o: {create: collName}, versionContext: testVersionContext}]
        }
    }]
}),
                             10296501);

rst.stopSet();
