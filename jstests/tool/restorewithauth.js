/* SERVER-4972
 * Test for mongorestore on server with --auth allows restore without credentials of colls
 * with no index
 */
/*
 * 1) Start mongo without auth.
 * 2) Write to collection
 * 3) Take dump of the collection using mongodump.
 * 4) Drop the collection.
 * 5) Stop mongod from step 1.
 * 6) Restart mongod with auth.
 * 7) Add admin user to kick authentication
 * 8) Try restore without auth credentials. The restore should fail
 * 9) Try restore with correct auth credentials. The restore should succeed this time.
 */

baseName = "jstests_restorewithauth";
var conn = MongoRunner.runMongod({nojournal: "", bind_ip: "127.0.0.1"});

// write to ns foo.bar
var foo = conn.getDB("foo");
for (var i = 0; i < 4; i++) {
    foo["bar"].save({"x": i});
    foo["baz"].save({"x": i});
}

// make sure the collection exists
var collNames = foo.getCollectionNames();
assert.neq(-1, collNames.indexOf("bar"), "bar collection doesn't exist");

// make sure it has no index except _id
assert.eq(foo.bar.getIndexes().length, 1);
assert.eq(foo.baz.getIndexes().length, 1);

foo.bar.createIndex({x: 1});
assert.eq(foo.bar.getIndexes().length, 2);
assert.eq(foo.baz.getIndexes().length, 1);

// get data dump
var dumpdir = MongoRunner.dataDir + "/restorewithauth-dump1/";
resetDbpath(dumpdir);

var exitCode = MongoRunner.runMongoTool("mongodump", {
    db: "foo",
    host: "127.0.0.1:" + conn.port,
    out: dumpdir,
});
assert.eq(0, exitCode, "mongodump failed to dump data from mongod without auth enabled");

// now drop the db
foo.dropDatabase();

// stop mongod
MongoRunner.stopMongod(conn);

// start mongod with --auth
conn = MongoRunner.runMongod({auth: "", nojournal: "", bind_ip: "127.0.0.1"});

// admin user
var admin = conn.getDB("admin");
admin.createUser({user: "admin", pwd: "admin", roles: jsTest.adminUserRoles});
admin.auth("admin", "admin");

var foo = conn.getDB("foo");

// make sure no collection with the same name exists
collNames = foo.getCollectionNames();
assert.eq(-1, collNames.indexOf("bar"), "bar collection already exists");
assert.eq(-1, collNames.indexOf("baz"), "baz collection already exists");

// now try to restore dump
exitCode = MongoRunner.runMongoTool("mongorestore", {
    host: "127.0.0.1:" + conn.port,
    dir: dumpdir,
    verbose: 5,
});
assert.neq(0,
           exitCode,
           "mongorestore shouldn't have been able to restore data to mongod with auth enabled");

// make sure that the collection isn't restored
collNames = foo.getCollectionNames();
assert.eq(-1, collNames.indexOf("bar"), "bar collection was restored");
assert.eq(-1, collNames.indexOf("baz"), "baz collection was restored");

// now try to restore dump with correct credentials
exitCode = MongoRunner.runMongoTool("mongorestore", {
    host: "127.0.0.1:" + conn.port,
    db: "foo",
    authenticationDatabase: "admin",
    username: "admin",
    password: "admin",
    dir: dumpdir + "foo/",
    verbose: 5,
});
assert.eq(0, exitCode, "mongorestore failed to restore data to mongod with auth enabled");

// make sure that the collection was restored
collNames = foo.getCollectionNames();
assert.neq(-1, collNames.indexOf("bar"), "bar collection was not restored");
assert.neq(-1, collNames.indexOf("baz"), "baz collection was not restored");

// make sure the collection has 4 documents
assert.eq(foo.bar.count(), 4);
assert.eq(foo.baz.count(), 4);

foo.dropDatabase();

foo.createUser({user: 'user', pwd: 'password', roles: jsTest.basicUserRoles});

// now try to restore dump with foo database credentials
exitCode = MongoRunner.runMongoTool("mongorestore", {
    host: "127.0.0.1:" + conn.port,
    db: "foo",
    username: "user",
    password: "password",
    dir: dumpdir + "foo/",
    verbose: 5,
});
assert.eq(0, exitCode, "mongorestore failed to restore the 'foo' database");

// make sure that the collection was restored
collNames = foo.getCollectionNames();
assert.neq(-1, collNames.indexOf("bar"), "bar collection was not restored");
assert.neq(-1, collNames.indexOf("baz"), "baz collection was not restored");
assert.eq(foo.bar.count(), 4);
assert.eq(foo.baz.count(), 4);
assert.eq(foo.bar.getIndexes().length + foo.baz.getIndexes().length,
          3);  // _id on foo, _id on bar, x on foo

MongoRunner.stopMongod(conn);
