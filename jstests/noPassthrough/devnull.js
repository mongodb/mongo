(function() {
    var merizo = MerizoRunner.runMerizod({storageEngine: "devnull"});

    db = merizo.getDB("test");

    res = db.foo.insert({x: 1});
    assert.eq(1, res.nInserted, tojson(res));

    // Skip collection validation during stopMerizod if invalid storage engine.
    TestData.skipCollectionAndIndexValidation = true;

    MerizoRunner.stopMerizod(merizo);
}());
