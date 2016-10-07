// SERVER-24933 3.2 clean shutdown left entries in the oplog that hadn't been applied. This could
// lead to data loss following a downgrade to 3.0. This tests that following a clean shutdown all
// ops in the oplog have been applied. This only applies to 3.2 and is not a requirement for 3.4.
// WARNING: this test does not always fail deterministically. It is possible for the bug to be
// present without this test failing. In particular if the rst.stop(1) doesn't execute mid-batch,
// it isn't actually testing this bug. However, if the test fails there is definitely a bug.
//
// @tags: [requires_persistence]
(function() {
    "use strict";

    var rst = new ReplSetTest({
        name: "name",
        nodes: 2,
    });

    rst.startSet();
    var conf = rst.getReplSetConfig();
    conf.members[1].votes = 0;
    conf.members[1].priority = 0;
    printjson(conf);
    rst.initiate(conf);

    var primary = rst.getPrimary();  // Waits for PRIMARY state.
    var slave = rst.nodes[1];

    // Stop replication on the secondary.
    assert.commandWorked(
        slave.adminCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'alwaysOn'}));

    // Prime the main collection.
    primary.getCollection("test.coll").insert({_id: -1});

    // Start a w:2 write that will block until replication is resumed.
    var waitForReplStart = startParallelShell(function() {
        printjson(assert.writeOK(db.getCollection('side').insert({}, {writeConcern: {w: 2}})));
    }, primary.host.split(':')[1]);

    // Insert a lot of data in increasing order to test.coll.
    var op = primary.getCollection("test.coll").initializeUnorderedBulkOp();
    for (var i = 0; i < 100 * 1000; i++) {
        op.insert({_id: i});
    }
    assert.writeOK(op.execute());

    // Resume replication and wait for ops to start replicating, then do a clean shutdown on the
    // secondary.
    assert.commandWorked(slave.adminCommand({configureFailPoint: 'rsSyncApplyStop', mode: 'off'}));
    waitForReplStart();
    sleep(100);  // wait a bit to increase the chances of killing mid-batch.
    rst.stop(1);

    // Restart the secondary as a standalone node.
    var options = slave.savedOptions;
    options.noCleanData = true;
    delete options.replSet;
    var conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, "secondary failed to start");

    // Following clean shutdown of a node, the oplog must exactly match the applied operations.
    // Additionally, the begin field must not be in the minValid document, the ts must match the
    // top of the oplog (SERVER-25353), and the oplogDeleteFromPoint must be null (SERVER-7200 and
    // SERVER-25071).
    var oplogDoc = conn.getCollection('local.oplog.rs')
                       .find({ns: 'test.coll'})
                       .sort({$natural: -1})
                       .limit(1)[0];
    var collDoc = conn.getCollection('test.coll').find().sort({_id: -1}).limit(1)[0];
    var minValidDoc =
        conn.getCollection('local.replset.minvalid').find().sort({$natural: -1}).limit(1)[0];
    printjson({oplogDoc: oplogDoc, collDoc: collDoc, minValidDoc: minValidDoc});
    try {
        assert.eq(collDoc._id, oplogDoc.o._id);
        assert(!('begin' in minValidDoc), 'begin in minValidDoc');
        assert.eq(minValidDoc.ts, oplogDoc.ts);
        if ('oplogDeleteFromPoint' in minValidDoc) {
            // If present it must be the null timestamp.
            assert.eq(minValidDoc.oplogDeleteFromPoint, Timestamp());
        }
    } catch (e) {
        jsTest.log(
            "Look above and make sure clean shutdown finished without resorting to SIGKILL." +
            "\nUnfortunately that currently doesn't fail the test.");
        throw e;
    }

    rst.stopSet();
})();
