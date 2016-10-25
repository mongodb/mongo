/**
 * Checks that top removes entries after dropping a collection or database.
 * TODO(SERVER-21167): Move this test from noPassthrough to core.
 */
(function() {
    "use strict";

    let topDB = db.getSiblingDB("topdrop");
    assert.commandWorked(topDB.dropDatabase());

    // Asserts that the output of top contains exactly these collection entries for topDB.
    function checkTopEntries(expectedEntries) {
        let res = topDB.adminCommand("top");
        assert.commandWorked(res, "Failed to run the top command");

        let entriesInTop = Object.keys(res.totals).filter(function(ns) {
            return ns.startsWith(topDB.getName() + ".");
        });
        let expectedEntryNames = expectedEntries.map(function(coll) {
            return coll.getFullName();
        });

        const entriesAreEqual = friendlyEqual(entriesInTop.sort(), expectedEntryNames.sort());
        if (!entriesAreEqual) {
            // TODO(SERVER-26750): This block can be removed once SERVER-26750 is resolved.
            jsTest.log("Warning: expected to see " + tojson(expectedEntryNames) +
                       " in top, but got " + tojson(entriesInTop));

            assert.lt(expectedEntryNames.length,
                      entriesInTop.length,
                      "Fewer entries in top than expected; got " + tojson(entriesInTop) +
                          " but expected " + tojson(expectedEntryNames) + "\nFull top output:\n" +
                          tojson(res.totals));

            // We allow an unexpected entry in top if the insert counter has been cleared. This is
            // probably due to a background job releasing an AutoGetCollectionForRead for that
            // collection.
            entriesInTop.forEach(function(coll) {
                if (expectedEntryNames.includes(coll)) {
                    return;
                }

                let topStats = res.totals[coll];
                assert.eq(0,
                          res.totals[coll].insert.count,
                          coll + " has unexpected insert entries in top. Full top output:\n" +
                              tojson(res.totals));
            });
        }
    }

    // Create a few entries in top.
    assert.writeOK(topDB.coll1.insert({}));
    assert.writeOK(topDB.coll2.insert({}));
    assert.writeOK(topDB.coll3.insert({}));
    checkTopEntries([topDB.coll1, topDB.coll2, topDB.coll3]);

    // Check that dropping a collection removes that collection but leaves the others.
    assert.commandWorked(topDB.runCommand({drop: "coll3"}));
    checkTopEntries([topDB.coll1, topDB.coll2]);

    // Check that dropping the database removes the remaining collections.
    assert.commandWorked(topDB.dropDatabase());
    checkTopEntries([]);
}());
