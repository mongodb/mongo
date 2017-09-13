// SERVER-7200 On startup, replica set nodes delete oplog state past the oplog delete point and
// apply any remaining unapplied ops before coming up as a secondary. This test specifically tests
// having an update and a delete of the same document in the same batch. This is a bit of an edge
// case because if the delete has been applied already, the update won't find any documents.
//
// @tags: [requires_persistence]
(function() {
    "use strict";

    var ns = "test.coll";
    var id = ObjectId();

    var rst = new ReplSetTest({
        nodes: 1,
    });

    rst.startSet();
    rst.initiate();

    var conn = rst.getPrimary();  // Waits for PRIMARY state.

    // Do the insert update and delete operations.
    var coll = conn.getCollection(ns);
    assert.writeOK(coll.insert({_id: id}));
    assert.writeOK(coll.update({_id: id}, {$inc: {a: 1}}));
    assert.writeOK(coll.remove({_id: id}));
    assert.eq(coll.findOne({_id: id}), null);

    // Set the appliedThrough point back to the insert so the update and delete are replayed.
    conn = rst.restart(0, {noReplSet: true});  // Restart as a standalone node.
    assert.neq(null, conn, "failed to restart");
    var oplog = conn.getCollection('local.oplog.rs');
    oplog.find().forEach(printjsononeline);
    assert.eq(oplog.count({ns: ns, op: 'i'}), 1);
    var insertOp = oplog.findOne({ns: ns, op: 'i'});
    var term = 't' in insertOp ? insertOp.t : -1;
    var minValidColl = conn.getCollection('local.replset.minvalid');
    assert.writeOK(minValidColl.update({}, {$set: {begin: {ts: insertOp.ts, t: term}}}));
    printjson({minValidDoc: minValidColl.findOne()});

    // Make sure it starts up fine again and doesn't have the document.
    conn = rst.restart(0);    // Restart in replSet mode again.
    conn = rst.getPrimary();  // Waits for PRIMARY state.
    coll = conn.getCollection(ns);
    assert.eq(coll.findOne({_id: id}), null);

    rst.stopSet();
})();
