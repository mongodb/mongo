mydb = db.getSisterDB( "repair_test1" )

t = mydb.jstests_repair;
t.drop();

t.save( { i:1 } );
doc = t.findOne();
t.ensureIndex( { i : 1 } );
assert.eq( 2, t.getIndexes().length );
ex = t.find( { i : 1 } ).explain();

assert.commandWorked( mydb.repairDatabase() );

v = t.validate();
assert( v.valid , "not valid! " + tojson( v ) );

assert.eq( 1, t.count() );
assert.eq( doc, t.findOne() );

assert.eq( 2, t.getIndexes().length, tojson( t.getIndexes() ) );
assert.eq( ex, t.find( { i : 1 } ).explain() );
