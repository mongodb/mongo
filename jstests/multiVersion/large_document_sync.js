/**
 * Tests syncing large objects from a 3.0 node to a 3.2 node. In particular, this test was designed
 * to stress the logic for upconverting an OP_REPLY that contains more than 16MB worth of documents.
 *
 * This test was designed to reproduce SERVER-26182.
 */
(function() {
    // Create a replica set with one "3.0" node and one "3.2" node.
    var replSetName = "testset";
    var nodes = [{binVersion: "3.0"}, {binVersion: "latest"}];

    var rst = ReplSetTest({name: replSetName, nodes: nodes, nodeOptions: {vv: ''}});
    rst.startSet();

    // Rig the election so that the 3.0 node becomes primary.
    var replSetConfig = rst.getReplSetConfig();
    replSetConfig.members[1].priority = 0;
    rst.initiate(replSetConfig);

    var primaryDB = rst.getPrimary().getDB("test");

    primaryDB.c.drop();

    var docCloseTo1MB = {
        x: new Array(900 * 1024).join("x")
    };
    assert.gte(Object.bsonsize(docCloseTo1MB), 0.5 * 1024 * 1024);
    assert.lt(Object.bsonsize(docCloseTo1MB), 1 * 1024 * 1024);

    var docCloseTo4MB = {
        x: new Array(3.5 * 1024 * 1024).join("x")
    };
    assert.gte(Object.bsonsize(docCloseTo4MB), 3 * 1024 * 1024);
    assert.lt(Object.bsonsize(docCloseTo4MB), 4 * 1024 * 1024);

    var docCloseTo16MB = {
        x: new Array(15.5 * 1024 * 1024).join("x")
    };
    assert.gte(Object.bsonsize(docCloseTo16MB), 15 * 1024 * 1024);
    assert.lt(Object.bsonsize(docCloseTo16MB), 16 * 1024 * 1024);

    assert.gt(Object.bsonsize(docCloseTo4MB) + Object.bsonsize(docCloseTo16MB), 16 * 1024 * 1024);

    assert.gt(Object.bsonsize(docCloseTo1MB) + Object.bsonsize(docCloseTo16MB), 16 * 1024 * 1024);

    rst.getPrimary().forceWriteMode("commands");
    // The first find has a threshold of 1MB, so put almost 1MB in, then a huge
    // document to make the total over 16MB.
    assert.writeOK(primaryDB.c.insert(docCloseTo1MB));
    assert.writeOK(primaryDB.c.insert(docCloseTo16MB));

    // The first getMore batch should contain both the ~4 MB and the ~16 MB
    // document, leading to more than 16 MB of user data in the batch.
    assert.writeOK(primaryDB.c.insert(docCloseTo4MB));
    assert.writeOK(primaryDB.c.insert(docCloseTo16MB));

    rst.awaitReplication();
    assert.commandWorked(rst.getSecondary().adminCommand({isMaster: 1}),
                         "expected secondary to survive syncing large documents");

    rst.stopSet();
})();
