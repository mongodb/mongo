print( " -- skipping repair test -- " );
quit();

t = db.jstests_repair;
t.drop();
t.save( { i:1 } );
db.repairDatabase();
assert( t.validate().valid );
