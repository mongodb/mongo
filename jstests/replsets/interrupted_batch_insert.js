// Tests the scenario described in SERVER-27534.
// 1. Send a single insert command with a large number of documents and the {ordered: true} option.
// 2. Force the thread processing the insert command to hang in between insert batches. (Inserts are
//    typically split into batches of 64, and the server yields locks between batches.)
// 3. Disconnect the original primary from the network, forcing another node to step up.
// 4. Insert a single document on the new primary.
// 5. Return the original primary to the network and force it to step up by disconnecting the
//    primary that replaced it. The original primary has to roll back any batches from step 1
//    that were inserted locally but did not get majority committed before the insert in step 4.
// 6. Unpause the thread performing the insert from step 1. If it continues to insert batches even
//    though there was a rollback, those inserts will violate the {ordered: true} option.

(function() {
    "use strict";

    load('jstests/libs/parallelTester.js');
    load("jstests/replsets/rslib.js");

    var name = "interrupted_batch_insert";
    var replTest = new ReplSetTest({name: name, nodes: 3, useBridge: true});
    var nodes = replTest.nodeList();

    var conns = replTest.startSet();
    replTest.initiate({
        _id: name,
        members: [
            {_id: 0, host: nodes[0]},
            {_id: 1, host: nodes[1]},
            {_id: 2, host: nodes[2], priority: 0}
        ]
    });

    // The test starts with node 0 as the primary.
    replTest.waitForState(replTest.nodes[0], ReplSetTest.State.PRIMARY);
    var primary = replTest.nodes[0];
    var collName = primary.getDB("db")[name].getFullName();

    var getParameterResult =
        primary.getDB("admin").runCommand({getParameter: 1, internalInsertMaxBatchSize: 1});
    assert.commandWorked(getParameterResult);
    const batchSize = getParameterResult.internalInsertMaxBatchSize;

    // Prevent any writes to node 0 (the primary) from replicating to nodes 1 and 2.
    stopServerReplication(conns[1]);
    stopServerReplication(conns[2]);

    // Allow the primary to insert the first 5 batches of documents. After that, the fail point
    // activates, and the client thread hangs until the fail point gets turned off.
    assert.commandWorked(primary.getDB("db").adminCommand(
        {configureFailPoint: "hangDuringBatchInsert", mode: {skip: 5}}));

    // In a background thread, issue an insert command to the primary that will insert 10 batches of
    // documents.
    var worker = new ScopedThread((host, collName, numToInsert) => {
        // Insert elements [{idx: 0}, {idx: 1}, ..., {idx: numToInsert - 1}].
        const docsToInsert = Array.from({length: numToInsert}, (_, i) => {
            return {idx: i};
        });
        var coll = new Mongo(host).getCollection(collName);
        assert.throws(
            () => coll.insert(docsToInsert,
                              {writeConcern: {w: "majority", wtimeout: 5000}, ordered: true}),
            [],
            "network error");
    }, primary.host, collName, 10 * batchSize);
    worker.start();

    // Wait long enough to guarantee that all 5 batches of inserts have executed and the primary is
    // hung on the "hangDuringBatchInsert" fail point.
    checkLog.contains(primary, "hangDuringBatchInsert fail point enabled");

    // Make sure the insert command is, in fact, running in the background.
    assert.eq(primary.getDB("db").currentOp({"command.insert": name, active: true}).inprog.length,
              1);

    // Completely isolate the current primary (node 0), forcing it to step down.
    conns[0].disconnect(conns[1]);
    conns[0].disconnect(conns[2]);

    // Wait for node 1, the only other eligible node, to become the new primary.
    replTest.waitForState(replTest.nodes[1], ReplSetTest.State.PRIMARY);
    assert.eq(replTest.nodes[1], replTest.getPrimary());

    restartServerReplication(conns[2]);

    // Issue a write to the new primary.
    var collOnNewPrimary = replTest.nodes[1].getCollection(collName);
    assert.writeOK(collOnNewPrimary.insert({singleDoc: 1}, {writeConcern: {w: "majority"}}));

    // Isolate node 1, forcing it to step down as primary, and reconnect node 0, allowing it to step
    // up again.
    conns[1].disconnect(conns[2]);
    conns[0].reconnect(conns[2]);

    // Wait for node 0 to become primary again.
    replTest.waitForState(primary, ReplSetTest.State.PRIMARY);
    assert.eq(replTest.nodes[0], replTest.getPrimary());

    // Allow the batch insert to continue.
    assert.commandWorked(primary.getDB("db").adminCommand(
        {configureFailPoint: "hangDuringBatchInsert", mode: "off"}));

    // Wait until the insert command is done.
    assert.soon(
        () =>
            primary.getDB("db").currentOp({"command.insert": name, active: true}).inprog.length ===
            0);

    worker.join();

    var docs = primary.getDB("db")[name].find({idx: {$exists: 1}}).sort({idx: 1}).toArray();

    // Any discontinuity in the "idx" values is an error. If an "idx" document failed to insert, all
    // the of "idx" documents after it should also have failed to insert, because the insert
    // specified {ordered: 1}. Note, if none of the inserts were successful, that's fine.
    docs.forEach((element, index) => {
        assert.eq(element.idx, index);
    });

    // Reconnect the remaining disconnected nodes, so we can exit.
    conns[0].reconnect(conns[1]);
    conns[1].reconnect(conns[2]);
    restartServerReplication(conns[1]);

    replTest.stopSet(15);
}());
