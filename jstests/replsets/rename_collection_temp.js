// In SERVER-23274, the renameCollection command was found to incorrectly swap the "dropTarget" and
// "stayTemp" arguments when run on a replica set. In this test, we check that the arguments are
// correctly propagated.

(function() {
    "use strict";

    function checkCollectionTemp(db, collName, expectedTempValue) {
        var collectionInformation = db.getCollectionInfos();

        var hasSeenCollection = false;
        for (var i = 0; i < collectionInformation.length; i++) {
            var collection = collectionInformation[i];

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

    var replTest = new ReplSetTest({name: 'renameCollectionTest', nodes: 2});
    var nodes = replTest.startSet();

    replTest.initiate();

    var master = replTest.getPrimary();

    // Create a temporary collection.
    var dbFoo = master.getDB("foo");

    assert.commandWorked(dbFoo.runCommand({create: "tempColl", temp: true}));
    checkCollectionTemp(dbFoo, "tempColl", true);

    // Rename the collection.
    assert.commandWorked(
        master.adminCommand({renameCollection: "foo.tempColl", to: "foo.permanentColl"}));

    // Confirm that it is no longer temporary.
    checkCollectionTemp(dbFoo, "permanentColl", false);

    replTest.awaitReplication();

    var secondary = replTest.getSecondary();
    var secondaryFoo = secondary.getDB("foo");

    secondaryFoo.permanentColl.setSlaveOk(true);

    // Get the information on the secondary to ensure it was replicated correctly.
    checkCollectionTemp(secondaryFoo, "permanentColl", false);

    // Check the behavior when the "dropTarget" flag is passed to renameCollection.
    dbFoo.permanentColl.drop();

    assert.commandWorked(dbFoo.runCommand({create: "tempColl", temp: true}));
    checkCollectionTemp(dbFoo, "tempColl", true);

    // Construct an empty collection that will be dropped on rename.
    assert.commandWorked(dbFoo.runCommand({create: "permanentColl"}));

    // Rename, dropping "permanentColl" and replacing it.
    assert.commandWorked(master.adminCommand(
        {renameCollection: "foo.tempColl", to: "foo.permanentColl", dropTarget: true}));

    checkCollectionTemp(dbFoo, "permanentColl", false);

    replTest.awaitReplication();

    checkCollectionTemp(secondaryFoo, "permanentColl", false);
}());
