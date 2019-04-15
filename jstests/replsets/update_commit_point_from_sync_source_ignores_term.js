/**
 * Tests that even if the sync source's lastOpCommitted is in a higher term than the node's
 * lastApplied, the node can update its own lastOpCommitted to its lastApplied.
 * @tags: [requires_majority_read_concern]
 */
(function() {
    "use strict";

    load("jstests/libs/write_concern_util.js");  // for [stop|restart]ServerReplication.

    const dbName = "test";
    const collName = "coll";

    // Set up a ReplSetTest where nodes only sync one oplog entry at a time.
    const rst = new ReplSetTest(
        {nodes: 5, useBridge: true, nodeOptions: {setParameter: "bgSyncOplogFetcherBatchSize=1"}});
    rst.startSet();
    const config = rst.getReplSetConfig();
    // Ban chaining and prevent elections.
    config.settings = {chainingAllowed: false, electionTimeoutMillis: 12 * 60 * 60 * 1000};
    rst.initiate(config);

    const nodeA = rst.nodes[0];
    const nodeB = rst.nodes[1];
    const nodeC = rst.nodes[2];
    const nodeD = rst.nodes[3];
    const nodeE = rst.nodes[4];

    jsTest.log("Node A is primary in term 1. Replicate a write to Node E that is not committed.");
    assert.eq(nodeA, rst.getPrimary());
    // Ensure Node E has a majority committed snapshot.
    assert.commandWorked(nodeA.getDB(dbName)[collName].insert({_id: "dummy"}));
    rst.awaitLastOpCommitted();
    stopServerReplication([nodeB, nodeC, nodeD]);
    assert.commandWorked(nodeA.getDB(dbName)[collName].insert({_id: "term 1, doc 1"}));
    rst.awaitReplication(undefined, undefined, [nodeE]);
    assert.eq(0,
              nodeE.getDB(dbName)[collName]
                  .find({_id: "term 1, doc 1"})
                  .readConcern("majority")
                  .itcount());

    jsTest.log("Disconnect Node E. Perform a new write.");
    nodeE.disconnect([nodeA, nodeB, nodeC, nodeD]);
    restartServerReplication([nodeB, nodeC, nodeD]);
    assert.commandWorked(nodeA.getDB(dbName)[collName].insert({_id: "term 1, doc 2"}));

    jsTest.log("Step up Node B in term 2. Commit a new write.");
    // Ensure Node B is caught up, so that it can become primary.
    rst.awaitReplication(undefined, undefined, [nodeB]);
    assert.commandWorked(nodeB.adminCommand({replSetStepUp: 1}));
    rst.waitForState(nodeA, ReplSetTest.State.SECONDARY);
    assert.eq(nodeB, rst.getPrimary());
    assert.commandWorked(
        nodeB.getDB(dbName)[collName].insert({_id: "term 2"}, {writeConcern: {w: "majority"}}));
    // Node E might sync from Node A or Node B. Ensure they both have the new commit point.
    rst.awaitLastOpCommitted(undefined, [nodeA]);

    jsTest.log("Allow Node E to replicate the last write from term 1.");
    // The stopReplProducerOnDocument failpoint ensures that Node E stops replicating before
    // applying the document {msg: "new primary"}, which is the first document of term 2. This
    // depends on the oplog fetcher batch size being 1.
    assert.commandWorked(nodeE.adminCommand({
        configureFailPoint: "stopReplProducerOnDocument",
        mode: "alwaysOn",
        data: {document: {msg: "new primary"}}
    }));
    nodeE.reconnect([nodeA, nodeB, nodeC, nodeD]);
    assert.soon(() => {
        return nodeE.getDB(dbName)[collName].find({_id: "term 1, doc 2"}).itcount() === 1;
    });
    assert.eq(0, nodeE.getDB(dbName)[collName].find({_id: "term 2"}).itcount());

    jsTest.log("Node E now knows that its first write is majority committed.");
    // It does not yet know that {_id: "term 1, doc 2"} is committed. Its last batch was {_id: "term
    // 1, doc 2"}. The sync source's lastOpCommitted was in term 2, so Node E updated its
    // lastOpCommitted to its lastApplied, which did not yet include {_id: "term 1, doc 2"}.
    assert.eq(1,
              nodeE.getDB(dbName)[collName]
                  .find({_id: "term 1, doc 1"})
                  .readConcern("majority")
                  .itcount());

    assert.commandWorked(
        nodeE.adminCommand({configureFailPoint: "stopReplProducerOnDocument", mode: "off"}));
    rst.stopSet();
}());
