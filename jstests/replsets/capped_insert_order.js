// Check that inserts to capped collections have the same order on primary and secondary.
// See SERVER-21483.

(function() {
    "use strict";

    var replTest = new ReplSetTest({name: 'capped_insert_order', nodes: 2});
    replTest.startSet();
    replTest.initiate();

    var master = replTest.getPrimary();
    var slave = replTest.liveNodes.slaves[0];

    var dbName = "db";
    var masterDb = master.getDB(dbName);
    var slaveDb = slave.getDB(dbName);

    var collectionName = "collection";
    var masterColl = masterDb[collectionName];
    var slaveColl = slaveDb[collectionName];

    // Making a large capped collection to ensure that every document fits.
    masterDb.createCollection(collectionName, {capped: true, size: 1024 * 1024});

    // Insert 1000 docs with _id from 0 to 999 inclusive.
    const nDocuments = 1000;
    var batch = masterColl.initializeOrderedBulkOp();
    for (var i = 0; i < nDocuments; i++) {
        batch.insert({_id: i});
    }
    assert.writeOK(batch.execute());
    replTest.awaitReplication();

    function checkCollection(coll) {
        assert.eq(coll.find().itcount(), nDocuments);

        var i = 0;
        coll.find().forEach(function(doc) {
            assert.eq(doc._id, i);
            i++;
        });
        assert.eq(i, nDocuments);
    }

    checkCollection(masterColl);
    checkCollection(slaveColl);

    replTest.stopSet();
})();
