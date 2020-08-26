// Check that inserts to capped collections have the same order on primary and secondary.
// See SERVER-21483.

(function() {
"use strict";

var replTest = new ReplSetTest({name: 'capped_insert_order', nodes: 2});
replTest.startSet();
replTest.initiate();

var primary = replTest.getPrimary();
var secondary = replTest.getSecondary();

var dbName = "db";
var primaryDb = primary.getDB(dbName);
var secondaryDb = secondary.getDB(dbName);

var collectionName = "collection";
var primaryColl = primaryDb[collectionName];
var secondaryColl = secondaryDb[collectionName];

// Making a large capped collection to ensure that every document fits.
primaryDb.createCollection(collectionName, {capped: true, size: 1024 * 1024});

// Insert 1000 docs with _id from 0 to 999 inclusive.
const nDocuments = 1000;
var batch = primaryColl.initializeOrderedBulkOp();
for (var i = 0; i < nDocuments; i++) {
    batch.insert({_id: i});
}
assert.commandWorked(batch.execute());
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

checkCollection(primaryColl);
checkCollection(secondaryColl);

replTest.stopSet();
})();
