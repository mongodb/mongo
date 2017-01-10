// Test that the collection created with the "convertToCapped" command inherits the default
// collation of the corresponding collection.
(function() {
    "use strict";

    let testDb = db.getSiblingDB("collation_convert_to_capped");
    let coll = testDb.coll;
    testDb.dropDatabase();

    // Create a collection with a non-simple default collation.
    assert.commandWorked(
        testDb.runCommand({create: coll.getName(), collation: {locale: "en", strength: 2}}));
    const originalCollectionInfos = testDb.getCollectionInfos({name: coll.getName()});
    assert.eq(originalCollectionInfos.length, 1, tojson(originalCollectionInfos));

    assert.writeOK(coll.insert({_id: "FOO"}));
    assert.writeOK(coll.insert({_id: "bar"}));
    assert.eq([{_id: "FOO"}],
              coll.find({_id: "foo"}).toArray(),
              "query should have performed a case-insensitive match");

    assert.commandWorked(testDb.runCommand({convertToCapped: coll.getName(), size: 4096}));
    const cappedCollectionInfos = testDb.getCollectionInfos({name: coll.getName()});
    assert.eq(cappedCollectionInfos.length, 1, tojson(cappedCollectionInfos));
    assert.eq(originalCollectionInfos[0].options.collation,
              cappedCollectionInfos[0].options.collation);
    assert.eq([{_id: "FOO"}], coll.find({_id: "foo"}).toArray());
})();
