// SERVER-4560 test

var dbTest = db.getSisterDB("DB_create_collection_fail_cleanup");
dbTest.dropDatabase();

assert(dbTest.getCollectionNames().length == 0);

// This create collection call should fail. It would leave the database in created state though.
var res = dbTest.createCollection("broken", {capped: true, size: -1});
assert.eq(false, res.ok);

dbTest.getCollectionNames().forEach(function(collName) {
    print(collName);
    assert(collName != 'broken');
});
