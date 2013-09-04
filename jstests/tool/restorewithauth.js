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

var port = allocatePorts(1)[0];
baseName = "jstests_restorewithauth";
var conn = startMongod( "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface",
                        "--nojournal", "--bind_ip", "127.0.0.1" );

// write to ns foo.bar
var foo = conn.getDB( "foo" );
for( var i = 0; i < 4; i++ ) {
    foo["bar"].save( { "x": i } );
    foo["baz"].save({"x": i});
}

// make sure the collection exists
assert.eq( foo.system.namespaces.count({name: "foo.bar"}), 1 )

//make sure it has no index except _id
assert.eq(foo.system.indexes.count(), 2);

foo.bar.createIndex({x:1});
assert.eq(foo.system.indexes.count(), 3);

// get data dump
var dumpdir = "/data/db/restorewithauth-dump1/";
resetDbpath( dumpdir );
x = runMongoProgram("mongodump", "--db", "foo", "-h", "127.0.0.1:"+port, "--out", dumpdir);

// now drop the db
foo.dropDatabase();

// stop mongod
stopMongod( port );

// start mongod with --auth
conn = startMongod( "--auth", "--port", port, "--dbpath", "/data/db/" + baseName, "--nohttpinterface",
                    "--nojournal", "--bind_ip", "127.0.0.1" );

// admin user
var admin = conn.getDB( "admin" )
admin.addUser( "admin" , "admin", jsTest.adminUserRoles );
admin.auth( "admin" , "admin" );

var foo = conn.getDB( "foo" )

// make sure no collection with the same name exists
assert.eq(foo.system.namespaces.count( {name: "foo.bar"}), 0);
assert.eq(foo.system.namespaces.count( {name: "foo.baz"}), 0);

// now try to restore dump
x = runMongoProgram( "mongorestore", "-h", "127.0.0.1:" + port,  "--dir" , dumpdir, "-vvvvv" );

// make sure that the collection isn't restored
assert.eq(foo.system.namespaces.count({name: "foo.bar"}), 0);
assert.eq(foo.system.namespaces.count({name: "foo.baz"}), 0);

// now try to restore dump with correct credentials
x = runMongoProgram( "mongorestore",
                     "-h", "127.0.0.1:" + port,
                     "-d", "foo",
                     "--authenticationDatabase=admin",
                     "-u", "admin",
                     "-p", "admin",
                     "--dir", dumpdir + "foo/",
                     "-vvvvv");

// make sure that the collection was restored
assert.eq(foo.system.namespaces.count({name: "foo.bar"}), 1);
assert.eq(foo.system.namespaces.count({name: "foo.baz"}), 1);

// make sure the collection has 4 documents
assert.eq(foo.bar.count(), 4);
assert.eq(foo.baz.count(), 4);

foo.dropDatabase();

// make sure that the collection is empty
assert.eq(foo.system.namespaces.count({name: "foo.bar"}), 0);
assert.eq(foo.system.namespaces.count({name: "foo.baz"}), 0);

foo.addUser('user', 'password', jsTest.basicUserRoles);

// now try to restore dump with foo database credentials
x = runMongoProgram("mongorestore",
                    "-h", "127.0.0.1:" + port,
                    "-d", "foo",
                    "-u", "user",
                    "-p", "password",
                    "--dir", dumpdir + "foo/",
                    "-vvvvv");

// make sure that the collection was restored
assert.eq(foo.system.namespaces.count({name: "foo.bar"}), 1);
assert.eq(foo.system.namespaces.count({name: "foo.baz"}), 1);
assert.eq(foo.bar.count(), 4);
assert.eq(foo.baz.count(), 4);
assert.eq(foo.system.indexes.count(), 3); // _id on foo, _id on bar, x on foo

stopMongod( port );
