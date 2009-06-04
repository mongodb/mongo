t = db.jstests_repair;
t.drop();
t.save( { i:1 } );
db.repairDatabase();
v = t.validate();
assert( v.valid , "not valid! " + tojson( v ) );
