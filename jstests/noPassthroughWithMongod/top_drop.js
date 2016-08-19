/**
 * Checks that top removes entries after dropping a collection or database.
 * TODO(SERVER-21167): Move this test from noPassthrough to core.
 */
(function() {
    let topDB = db.getSiblingDB("topdrop");
    assert.commandWorked(topDB.dropDatabase());

    // Asserts that the output of top contains exactly these collection entries for topDB.
    function checkTopEntries(expectedEntries) {
        let res = topDB.adminCommand("top");
        assert.commandWorked(res);
        let namesFromTotals = Object.keys(res.totals).filter(function(ns) {
            return ns.startsWith(topDB.getName() + ".");
        });
        let expectedEntryNames = expectedEntries.map(function(coll) {
            return coll.getFullName();
        });
        assert.eq(namesFromTotals.sort(), expectedEntryNames.sort());
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
