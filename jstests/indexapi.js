
t = db.indexapi;
t.drop();

key = { x : 1 };

db.system.indexes.insert( { ns : "test" , key : { x : 1 } , name : "x" } );
assert( db.getLastError().indexOf( "invalid" ) >= 0 , "Z1" );
