/**
 * This test creates a replica set and tries copying the local database. It expects an error on
 * the `copydb` command when it runs across an illegal namespace to copy, e.g:
 * `local.system.replset` -> `db2.system.replset`.
 * @tags: [requires_replication, requires_persistence]
 */
(function() {
    "use strict";
    var rst = new ReplSetTest({nodes: 1});

    rst.startSet();
    rst.initiate();

    var conn = rst.getPrimary();               // Waits for PRIMARY state.
    conn = rst.restart(0, {noReplSet: true});  // Restart as a standalone node.
    assert.neq(null, conn, "failed to restart");

    // Must drop the oplog in order to induce the correct error below.
    conn.getDB("local").oplog.rs.drop();

    var db1 = conn.getDB("local");
    var db2 = conn.getDB("db2");

    var res = db1.adminCommand({copydb: 1, fromdb: db1._name, todb: db2._name});

    assert.commandFailedWithCode(res, ErrorCodes.InvalidNamespace);
    assert.gt(res["errmsg"].indexOf("cannot write to 'db2.system.replset'"), -1);
    rst.stopSet();
})();
