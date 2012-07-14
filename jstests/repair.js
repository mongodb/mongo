mydb = db.getSisterDB( "repair_test1" )
t = mydb.jstests_repair;
t.drop();
t.save( { i:1 } );
assert.commandWorked( mydb.repairDatabase() );
v = t.validate();
assert( v.valid , "not valid! " + tojson( v ) );
