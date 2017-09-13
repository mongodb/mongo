(function() {
    var mongo = MongoRunner.runMongod({smallfiles: "", storageEngine: "devnull"});

    db = mongo.getDB("test");

    res = db.foo.insert({x: 1});
    assert.eq(1, res.nInserted, tojson(res));

    // Skip collection validation during stopMongod if invalid storage engine.
    TestData.skipCollectionAndIndexValidation = true;

    MongoRunner.stopMongod(mongo);
}());
