admin = db.getMongo().getDB( "admin" );

a = db.jstests_rename_a;
b = db.jstests_rename_b;
c = db.jstests_rename_c;

a.drop();
b.drop();
c.drop();

a.save( {a: 1} );
a.save( {a: 2} );
a.ensureIndex( {a:1} );
a.ensureIndex( {b:1} );

c.save( {a: 100} );
assert.commandFailed( admin.runCommand( {renameCollection:"test.jstests_rename_a", to:"test.jstests_rename_c"} ) );

assert.commandWorked( admin.runCommand( {renameCollection:"test.jstests_rename_a", to:"test.jstests_rename_b"} ) );
assert.eq( 0, a.find().count() );

assert.eq( 2, b.find().count() );
assert( db.system.namespaces.findOne( {name:"test.jstests_rename_b" } ) );
assert( !db.system.namespaces.findOne( {name:"test.jstests_rename_a" } ) );
assert.eq( 3, db.system.indexes.find( {ns:"test.jstests_rename_b"} ).count() );
assert.eq( 0, db.system.indexes.find( {ns:"test.jstests_rename_a"} ).count() );
assert( b.find( {a:1} ).explain().cursor.match( /^BtreeCursor/ ) );

// now try renaming a capped collection

a.drop();
b.drop();
c.drop();

// TODO: too many numbers hard coded here
// this test depends precisely on record size and hence may not be very reliable
// note we use floats to make sure numbers are represented as doubles for both SM and v8, since test relies on record size
db.createCollection( "jstests_rename_a", {capped:true,size:10000} );
for( i = 0.1; i < 10; ++i ) {
    a.save( { i: i } );
}
assert.commandWorked( admin.runCommand( {renameCollection:"test.jstests_rename_a", to:"test.jstests_rename_b"} ) );
assert.eq( 1, b.count( {i:9.1} ) );
for( i = 10.1; i < 250; ++i ) {
    b.save( { i: i } );
}

//res = b.find().sort({i:1});
//while (res.hasNext()) printjson(res.next());

assert.eq( 0, b.count( {i:9.1} ) );
assert.eq( 1, b.count( {i:19.1} ) );

assert( db.system.namespaces.findOne( {name:"test.jstests_rename_b" } ) );
assert( !db.system.namespaces.findOne( {name:"test.jstests_rename_a" } ) );
assert.eq( true, db.system.namespaces.findOne( {name:"test.jstests_rename_b"} ).options.capped );
