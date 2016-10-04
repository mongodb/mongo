// Test that lastOp is updated properly in the face of no-op writes and for writes that generate
// errors based on the preexisting data (e.g. duplicate key errors, but not parse errors).
// lastOp is used as the optime to wait for when write concern waits for replication.
(function() {
    var replTest = new ReplSetTest({name: 'testSet', nodes: 1});
    replTest.startSet();
    replTest.initiate();

    var primary = replTest.getPrimary();

    // Two connections
    var m1 = new Mongo(primary.host);
    var m2 = new Mongo(primary.host);

    // Do a write with m1, then a write with m2, then a no-op write with m1. m1 should have a lastOp
    // of m2's write.

    assert.writeOK(m1.getCollection("test.foo").insert({m1: 1}));
    var firstOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({m2: 99}));
    var secondOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // No-op update
    assert.writeOK(m1.getCollection("test.foo").update({m1: 1}, {$set: {m1: 1}}));
    var noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, secondOp);

    assert.writeOK(m1.getCollection("test.foo").remove({m1: 1}));
    var thirdOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({m2: 98}));
    var fourthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // No-op delete
    assert.writeOK(m1.getCollection("test.foo").remove({m1: 1}));
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, fourthOp);

    // Dummy write, for a new lastOp.
    assert.writeOK(m1.getCollection("test.foo").insert({m1: 99}));
    var fifthOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({m2: 97}));
    var sixthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // No-op find-and-modify delete
    m1.getCollection("test.foo").findAndModify({query: {m1: 1}, remove: 'true'});
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, sixthOp);

    assert.commandWorked(m1.getCollection("test.foo").createIndex({x: 1}));
    var seventhOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({m2: 96}));
    var eighthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // No-op create index.
    assert.commandWorked(m1.getCollection("test.foo").createIndex({x: 1}));
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, eighthOp);

    assert.writeOK(m1.getCollection("test.foo").insert({_id: 1, x: 1}));
    var ninthOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({m2: 991}));
    var tenthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // update with immutable field error
    assert.writeError(m1.getCollection("test.foo").update({_id: 1, x: 1}, {$set: {_id: 2}}));
    // "After applying the update to the document {_id: 1.0 , ...}, the (immutable) field '_id'
    // was found to have been altered to _id: 2.0"
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, tenthOp);

    assert.writeOK(m2.getCollection("test.foo").insert({m2: 992}));
    var eleventhOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // find-and-modify immutable field error
    try {
        m1.getCollection("test.foo")
            .findAndModify({query: {_id: 1, x: 1}, update: {$set: {_id: 2}}});
        // The findAndModify shell helper should throw.
        assert(false);
    } catch (e) {
        assert.eq(e.code, 66);
    }
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, eleventhOp);

    var bigString = new Array(3000).toString();
    assert.writeOK(m2.getCollection("test.foo").insert({m2: 994, m3: bigString}));

    // createIndex with a >1024 byte field fails.
    var twelfthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;
    assert.commandFailed(m1.getCollection("test.foo").createIndex({m3: 1}));
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, twelfthOp);

    // No-op insert
    assert.writeOK(m1.getCollection("test.foo").insert({_id: 5, x: 5}));
    var thirteenthOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.writeOK(m2.getCollection("test.foo").insert({m2: 991}));
    var fourteenthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    // Hits DuplicateKey error and fails insert -- no-op
    assert.writeError(m1.getCollection("test.foo").insert({_id: 5, x: 5}));
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    assert.eq(noOp, fourteenthOp);

    // Test update and delete failures in legacy write mode.
    m2.forceWriteMode('legacy');
    m1.forceWriteMode('legacy');
    m2.getCollection("test.foo").insert({m2: 995});
    var fifthteenthOp = m2.getCollection("test.foo").getDB().getLastErrorObj().lastOp;

    m1.getCollection("test.foo").remove({m1: 1});
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;
    assert.eq(noOp, fifthteenthOp);

    m1.getCollection("test.foo").update({m1: 1}, {$set: {m1: 4}});
    noOp = m1.getCollection("test.foo").getDB().getLastErrorObj().lastOp;
    assert.eq(noOp, fifthteenthOp);

})();
