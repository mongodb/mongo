(function() {
    "use strict";
    // This test ensures that CloneCollectionAsCapped()ing a nonexistent collection will not
    // cause the server to abort (SERVER-13750)

    var dbname = "clone_collection_as_capped_nonexistent";
    var testDb = db.getSiblingDB(dbname);
    testDb.dropDatabase();

    // Database does not exist here
    var res = testDb.runCommand({cloneCollectionAsCapped: 'foo', toCollection: 'bar', size: 1024});
    assert.eq(res.ok, 0, "cloning a nonexistent collection to capped should not have worked");
    var isSharded = (db.isMaster().msg == "isdbgrid");

    assert.eq(
        res.errmsg,
        isSharded ? "no such cmd: cloneCollectionAsCapped" : "database " + dbname + " not found",
        "converting a nonexistent to capped failed but for the wrong reason");

    // Database exists, but collection doesn't
    testDb.coll.insert({});

    var res = testDb.runCommand({cloneCollectionAsCapped: 'foo', toCollection: 'bar', size: 1024});
    assert.eq(res.ok, 0, "cloning a nonexistent collection to capped should not have worked");
    assert.eq(res.errmsg,
              isSharded ? "no such cmd: cloneCollectionAsCapped"
                        : "source collection " + dbname + ".foo does not exist",
              "converting a nonexistent to capped failed but for the wrong reason");
}());
