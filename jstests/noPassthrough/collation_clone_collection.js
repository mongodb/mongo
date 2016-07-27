/**
 * Tests that the "cloneCollection" command inherits the collection-default collation and that it is
 * used when filtering the source collection.
 */
(function() {
    "use strict";

    var source = MongoRunner.runMongod({});
    assert.neq(null, source, "mongod was unable to start up");

    var dest = MongoRunner.runMongod({});
    assert.neq(null, dest, "mongod was unable to start up");

    var sourceColl = source.getDB("test").collation;
    var destColl = dest.getDB("test").collation;

    assert.commandWorked(sourceColl.getDB().runCommand(
        {create: sourceColl.getName(), collation: {locale: "en", strength: 2}}));
    var sourceCollectionInfos = sourceColl.getDB().getCollectionInfos({name: sourceColl.getName()});

    assert.writeOK(sourceColl.insert({_id: "FOO"}));
    assert.writeOK(sourceColl.insert({_id: "bar"}));
    assert.eq([{_id: "FOO"}],
              sourceColl.find({_id: "foo"}).toArray(),
              "query should have performed a case-insensitive match");

    // Test that the "cloneCollection" command respects the collection-default collation.
    destColl.drop();
    assert.commandWorked(destColl.getDB().runCommand({
        cloneCollection: sourceColl.getFullName(),
        from: sourceColl.getMongo().host,
        query: {_id: "foo"}
    }));

    var destCollectionInfos = destColl.getDB().getCollectionInfos({name: destColl.getName()});
    assert.eq(sourceCollectionInfos, destCollectionInfos);
    assert.eq([{_id: "FOO"}], destColl.find({}).toArray());
})();
