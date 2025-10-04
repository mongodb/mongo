// Check that inserts to capped collections have the same order on primary and secondary.
// See SERVER-21483.

import {ReplSetTest} from "jstests/libs/replsettest.js";

let replTest = new ReplSetTest({name: jsTestName(), nodes: 2});
replTest.startSet();
replTest.initiate();

let primary = replTest.getPrimary();
let secondary = replTest.getSecondary();

let dbName = "db";
let primaryDb = primary.getDB(dbName);
let secondaryDb = secondary.getDB(dbName);

let collectionName = "collection";
let primaryColl = primaryDb[collectionName];
let secondaryColl = secondaryDb[collectionName];

// Making a large capped collection to ensure that every document fits.
primaryDb.createCollection(collectionName, {capped: true, size: 1024 * 1024});

// Insert 1000 docs with _id from 0 to 999 inclusive.
const nDocuments = 1000;
let batch = primaryColl.initializeOrderedBulkOp();
for (let i = 0; i < nDocuments; i++) {
    batch.insert({_id: i});
}
assert.commandWorked(batch.execute());
replTest.awaitReplication();

function checkCollection(coll) {
    assert.eq(coll.find().itcount(), nDocuments);

    let i = 0;
    coll.find().forEach(function (doc) {
        assert.eq(doc._id, i);
        i++;
    });
    assert.eq(i, nDocuments);
}

checkCollection(primaryColl);
checkCollection(secondaryColl);

replTest.stopSet();
