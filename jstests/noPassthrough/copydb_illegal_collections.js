/**
 * This test creates a replica set and tries copying the local database. It expects an error on
 * the `copydb` command when it runs across an illegal namespace to copy, e.g:
 * `local.system.replset` -> `db2.system.replset`.
 * @tags: [requires_replication, requires_persistence]
 */
(function() {
    "use strict";
    var rst = new ReplSetTest({nodes: 1});

    // Start as stand-alone node so that local.oplog.rs collection won't be created. In order to
    // make 'copydb' cmd to fail with 'ErrorCodes.InvalidNamespace`, it is necessary that local db
    // should not contain oplog collection.
    rst.start(0, {noReplSet: true});

    var conn = rst.getPrimary();  // Waits for PRIMARY state.

    var db1 = conn.getDB("local");
    var db2 = conn.getDB("db2");

    // Create system.replset collection manually.
    assert.commandWorked(db1.runCommand({create: 'system.replset'}));

    var res = db1.adminCommand({copydb: 1, fromdb: db1._name, todb: db2._name});

    assert.commandFailedWithCode(res, ErrorCodes.InvalidNamespace);
    assert.gt(res["errmsg"].indexOf("cannot write to 'db2.system.replset'"), -1);
    rst.stopSet();
})();
