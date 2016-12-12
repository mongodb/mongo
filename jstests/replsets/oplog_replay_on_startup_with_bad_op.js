// SERVER-7200 On startup, replica set nodes delete oplog state past the oplog delete point and
// apply any remaining unapplied ops before coming up as a secondary. If the op fails to apply, the
// server must fail to start up.
//
// @tags: [requires_persistence]
(function() {
    "use strict";

    var rst = new ReplSetTest({
        nodes: 1,
    });

    rst.startSet();
    rst.initiate();

    var conn = rst.getPrimary();               // Waits for PRIMARY state.
    conn = rst.restart(0, {noReplSet: true});  // Restart as a standalone node.
    assert.neq(null, conn, "failed to restart");

    var oplog = conn.getCollection('local.oplog.rs');
    var lastOplogDoc = conn.getCollection('local.oplog.rs').find().sort({$natural: -1}).limit(1)[0];
    var lastTs = lastOplogDoc.ts;
    var newTs = Timestamp(lastTs.t + 1, 1);
    var term = lastOplogDoc.t;
    if (typeof(term) == 'undefined') {
        term = -1;  // Use a dummy term for PV0.
    }

    assert.writeOK(oplog.insert({
        ts: newTs,
        t: term,
        h: 1,
        op: 'c',
        ns: 'somedb.$cmd',
        o: {thereIsNoCommandWithThisName: 1},
    }));

    var injectedMinValidDoc = {
        _id: ObjectId(),

        // minvalid:
        ts: newTs,
        t: term,

        // appliedThrough
        begin: {
            ts: lastTs,
            t: term,
        },

        oplogDeleteFromPoint: Timestamp(),
    };

    // This weird mechanism is the only way to bypass mongod's attempt to fill in null
    // Timestamps.
    var minValidColl = conn.getCollection('local.replset.minvalid');
    assert.writeOK(minValidColl.remove({}));
    assert.writeOK(minValidColl.update({}, {$set: injectedMinValidDoc}, {upsert: true}));
    assert.eq(minValidColl.findOne(),
              injectedMinValidDoc,
              "If the Timestamps differ, the server may be filling in the null timestamps");

    assert.throws(() => rst.restart(0));  // Restart in replSet mode again.
    print('\n\n\t\t^^^^^^^^  This was supposed to fassert ^^^^^^^^^^^\n\n');
})();
