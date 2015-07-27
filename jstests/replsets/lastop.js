// Test that lastOp is updated properly in the face of no-op writes.
// lastOp is used as the optime to wait for when write concern waits for replication.
(function () {
    var replTest = new ReplSetTest({ name: 'testSet', nodes: 1 });
    replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();

    // Two connections
    var m1 = new Mongo(primary.host);
    var m2 = new Mongo(primary.host);

    // Do a write with m1, then a write with m2, then a no-op write with m1. m1 should have a lastOp
    // of m2's write.
    
    assert.writeOK(m1.getCollection("test.foo").insert({ m1 : 1 }));
    var firstOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;
    
    assert.writeOK(m2.getCollection("test.foo").insert({ m2 : 99 }));
    var secondOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // No-op update
    assert.writeOK(m1.getCollection("test.foo").update({ m1 : 1 }, { $set: { m1 : 1 }}));
    var noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, secondOp);

    
    assert.writeOK(m1.getCollection("test.foo").remove({ m1 : 1 }));
    var thirdOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({ m2 : 98 }));
    var fourthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // No-op delete
    assert.writeOK(m1.getCollection("test.foo").remove({ m1 : 1 }));
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, fourthOp);

    
    // Dummy write, for a new lastOp.
    assert.writeOK(m1.getCollection("test.foo").insert({ m1 : 99 }));
    var fifthOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({ m2 : 97 }));
    var sixthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // No-op find-and-modify delete
    m1.getCollection("test.foo").findAndModify( { query: { m1 : 1 } , remove: 'true'} );
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, sixthOp);

    assert.commandWorked(m1.getCollection("test.foo").createIndex({x:1}));
    var seventhOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({ m2 : 96 }));
    var eighthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // No-op create index.
    assert.commandWorked(m1.getCollection("test.foo").createIndex({x:1}));
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, eighthOp);
})();
