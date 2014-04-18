// ***************************************************************
// rename7.js
// Test renameCollection functionality across different databases.
// ***************************************************************

// Set up namespaces a and b.
admin = db.getMongo().getDB( "admin" );
db_a = db.getMongo().getDB( "db_a" );
db_b = db.getMongo().getDB( "db_b" );
a = db_a.rename7;
b = db_b.rename7;

a.drop();
b.drop();

// Put some documents and indexes in a.
a.save( {a: 1} );
a.save( {a: 2} );
a.save( {a: 3} );
a.ensureIndex( {a: 1} );
a.ensureIndex( {b: 1} );

assert.commandWorked( admin.runCommand( {renameCollection: "db_a.rename7", to: "db_b.rename7"} ) );

assert.eq( 0, a.find().count() );
assert( !db_a.system.namespaces.findOne( {name: "db_a.rename7"} ) );

assert.eq( 3, b.find().count() );
assert( db_b.system.namespaces.findOne( {name: "db_b.rename7"} ) );
assert( b.find( {a: 1} ).explain().cursor.match( /^BtreeCursor/ ) );

a.drop();
b.drop();

// Capped collection testing.
db_a.createCollection( "rename7_capped", {capped:true, size:10000} );
a = db_a.rename7_capped;
b = db_b.rename7_capped;

a.save( {a: 1} );
a.save( {a: 2} );
a.save( {a: 3} );

assert.commandWorked( admin.runCommand( {renameCollection: "db_a.rename7_capped",
                                         to: "db_b.rename7_capped"} ) );

assert.eq( 0, a.find().count() );
assert( !db_a.system.namespaces.findOne( {name: "db_a.rename7_capped"} ) );

assert.eq( 3, b.find().count() );
assert( db_b.system.namespaces.findOne( {name: "db_b.rename7_capped"} ) );
assert.eq( true, db_b.system.namespaces.findOne( {name:"db_b.rename7_capped"} ).options.capped );
assert.eq( 12288, b.stats().storageSize );

a.drop();
b.drop();
