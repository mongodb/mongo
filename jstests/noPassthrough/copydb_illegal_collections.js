/**
 * This test creates a replica set and tries copying the local database. It expects an error on
 * the `copydb` command when it runs across an illegal namespace to copy, e.g:
 * `local.system.replset` -> `db2.system.replset`.
 */
(function() {
    "use strict";
    var numNodes = 3;
    var rst = new ReplSetTest({nodes: numNodes});

    rst.startSet();
    rst.initiate();

    var db1 = rst.getPrimary().getDB("local");
    var db2 = rst.getPrimary().getDB("db2");

    var res = db1.adminCommand({copydb: 1, fromdb: db1._name, todb: db2._name});
    assert.commandFailedWithCode(res, ErrorCodes.InvalidNamespace);
    assert.gt(res["errmsg"].indexOf("cannot write to 'db2.system.replset'"), -1);
    rst.awaitReplication();
})();
