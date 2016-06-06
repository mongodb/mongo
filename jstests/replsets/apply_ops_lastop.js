//
// Test that last optime is updated properly when the applyOps command fails and there's a no-op.
// lastOp is used as the optime to wait for when write concern waits for replication.
//

(function() {
    "use strict";

    var rs = new ReplSetTest({name: "applyOpsOptimeTest", nodes: 3});
    rs.startSet();
    var nodes = rs.nodeList();
    rs.initiate({
        "_id": "applyOpsOptimeTest",
        "members": [
            {"_id": 0, "host": nodes[0]},
            {"_id": 1, "host": nodes[1]},
            {"_id": 2, "host": nodes[2], "arbiterOnly": true}
        ]
    });
    var primary = rs.getPrimary();
    var db = primary.getDB('foo');
    var coll = primary.getCollection('foo.bar');
    // Two connections
    var m1 = new Mongo(primary.host);
    var m2 = new Mongo(primary.host);

    var insertApplyOps = [{op: "i", ns: 'foo.bar', o: {_id: 1, a: "b"}}];
    var deleteApplyOps = [{op: "d", ns: 'foo.bar', o: {_id: 1, a: "b"}}];
    var badPreCondition = [{ns: 'foo.bar', q: {_id: 10, a: "aaa"}, res: {a: "aaa"}}];
    var majorityWriteConcern = {w: 'majority', wtimeout: 30000};

    // Set up some data
    assert.writeOK(coll.insert({x: 1}));  // creating the collection so applyOps works
    assert.commandWorked(
        m1.getDB('foo').runCommand({applyOps: insertApplyOps, writeConcern: majorityWriteConcern}));
    var insertOp = m1.getDB('foo').getLastErrorObj('majority', 30000).lastOp;

    // No-op applyOps
    var res = m2.getDB('foo').runCommand({
        applyOps: deleteApplyOps,
        preCondition: badPreCondition,
        writeConcern: majorityWriteConcern
    });
    assert.commandFailed(res, "The applyOps command was expected to fail, but instead succeeded.");
    assert.eq(
        res.errmsg, "preCondition failed", "The applyOps command failed for the wrong reason.");
    var noOp = m2.getDB('foo').getLastErrorObj('majority', 30000).lastOp;

    // Check that each connection has the same last optime
    assert.eq(noOp,
              insertOp,
              "The connections' last optimes do " +
                  "not match: applyOps failed to update lastop on no-op");

    rs.stopSet();

})();
