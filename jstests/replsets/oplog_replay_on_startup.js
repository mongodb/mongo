// SERVER-7200 On startup, replica set nodes delete oplog state past the oplog delete point and
// apply any remaining unapplied ops before coming up as a secondary.
//
// @tags: [requires_persistence]
(function() {
    "use strict";

    var rst = new ReplSetTest({
        name: "server_7200",
        nodes: 1,
    });

    rst.startSet();
    rst.initiate();

    var conn = rst.getPrimary();  // Waits for PRIMARY state.
    var ns = "test.coll";

    conn.getCollection(ns).insert({_id: 1});

    conn = rst.restart(0, {noReplSet: true});  // Restart as a standalone node.
    assert.neq(null, conn, "failed to restart");

    var originalOp = conn.getCollection('local.oplog.rs').find().sort({$natural: -1}).limit(1)[0];
    if (!('t' in originalOp)) {
        originalOp.t = -1;  // Add the dummy term for PV0.
    }
    var tsPlus = (inc) => Timestamp(originalOp.ts.t, originalOp.ts.i + inc);

    var injectedOps = [
        // These will be applied.
        {ts: tsPlus(1), t: originalOp.t, h: 1, op: 'i', ns: ns, o: {_id: 2}},
        {ts: tsPlus(2), t: originalOp.t, h: 1, op: 'i', ns: ns, o: {_id: 3}},

        // These will be deleted.
        {ts: tsPlus(3), t: originalOp.t, h: 1, op: 'i', ns: ns, o: {_id: 4}},
        {ts: tsPlus(4), t: originalOp.t, h: 1, op: 'i', ns: ns, o: {_id: 5}},
    ];

    var injectedMinValidDoc = {
        // minvalid:
        ts: injectedOps[1].ts,
        t: injectedOps[1].t,

        // appliedThrough
        begin: {
            ts: originalOp.ts,
            t: originalOp.t,
        },

        oplogDeleteFromPoint: injectedOps[2].ts,
    };

    conn.getCollection("local.oplog.rs").insert(injectedOps);
    conn.getCollection("local.replset.minvalid").update({}, injectedMinValidDoc, {upsert: true});

    rst.restart(0);  // Restart in replSet mode again.

    var conn = rst.getPrimary();  // Waits for PRIMARY state.
    assert.neq(null, conn, "failed to restart");

    // Ensure the oplog has the entries it should have and none that it shouldn't.
    assert.eq(conn.getCollection('local.oplog.rs').distinct('o._id', {ns: ns, op: 'i'}), [1, 2, 3]);

    // Ensure that all ops that should have been applied were.
    assert.eq(conn.getCollection(ns).distinct('_id'), [1, 2, 3]);

    rst.stopSet();
})();
