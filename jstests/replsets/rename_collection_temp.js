// In SERVER-23274, the renameCollection command was found to incorrectly swap the "dropTarget" and
// "stayTemp" arguments when run on a replica set. In this test, we check that the arguments are
// correctly propagated.
// @tags: [requires_replication]

import {ReplSetTest} from "jstests/libs/replsettest.js";

function checkCollectionTemp(db, collName, expectedTempValue) {
    let collectionInformation = db.getCollectionInfos();

    let hasSeenCollection = false;
    for (let i = 0; i < collectionInformation.length; i++) {
        let collection = collectionInformation[i];

        if (collection.name === collName) {
            hasSeenCollection = true;

            if (expectedTempValue) {
                // We expect this collection to be temporary.
                assert.eq(collection.options.temp, true);
            } else {
                // We expect this collection to be permanent, thus the temp option will not show
                // up.
                assert.isnull(collection.options.temp);
            }
        }
    }
}

let replTest = new ReplSetTest({name: "renameCollectionTest", nodes: 2});
let nodes = replTest.startSet();

replTest.initiate();

let primary = replTest.getPrimary();

// Create a temporary collection.
let dbFoo = primary.getDB("foo");

assert.commandWorked(
    dbFoo.runCommand({applyOps: [{op: "c", ns: dbFoo.getName() + ".$cmd", o: {create: "tempColl", temp: true}}]}),
);
checkCollectionTemp(dbFoo, "tempColl", true);

// Rename the collection.
assert.commandWorked(primary.adminCommand({renameCollection: "foo.tempColl", to: "foo.permanentColl"}));

// Confirm that it is no longer temporary.
checkCollectionTemp(dbFoo, "permanentColl", false);

replTest.awaitReplication();

let secondary = replTest.getSecondary();
let secondaryFoo = secondary.getDB("foo");

secondaryFoo.permanentColl.setSecondaryOk();

// Get the information on the secondary to ensure it was replicated correctly.
checkCollectionTemp(secondaryFoo, "permanentColl", false);

// Check the behavior when the "dropTarget" flag is passed to renameCollection.
dbFoo.permanentColl.drop();

assert.commandWorked(
    dbFoo.runCommand({applyOps: [{op: "c", ns: dbFoo.getName() + ".$cmd", o: {create: "tempColl", temp: true}}]}),
);
checkCollectionTemp(dbFoo, "tempColl", true);

// Construct an empty collection that will be dropped on rename.
assert.commandWorked(dbFoo.runCommand({create: "permanentColl"}));

// Rename, dropping "permanentColl" and replacing it.
assert.commandWorked(
    primary.adminCommand({renameCollection: "foo.tempColl", to: "foo.permanentColl", dropTarget: true}),
);

checkCollectionTemp(dbFoo, "permanentColl", false);

replTest.awaitReplication();

checkCollectionTemp(secondaryFoo, "permanentColl", false);

replTest.stopSet();
