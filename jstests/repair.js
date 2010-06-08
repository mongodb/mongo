t = db.jstests_repair;
t.drop();
t.save( { i:1 } );
assert.commandWorked( db.repairDatabase() );
v = t.validate();
assert( v.valid , "not valid! " + tojson( v ) );
