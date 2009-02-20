/**
 *   Tests for the db object enhancement
 */

dd = function( x ){
    //print( x );
}

dd( "a" );


dd( "b" ); 

/*
 *  be sure the public collection API is complete
 */
assert(db.createCollection , "createCollection" );
assert(db.getProfilingLevel , "getProfilingLevel" );
assert(db.setProfilingLevel , "setProfilingLevel" );
assert(db.dbEval , "dbEval" );
assert(db.group , "group" );

dd( "c" );

/*
 * test createCollection
 */
 
db.getCollection( "test" ).drop();
db.getCollection( "system.namespaces" ).find().forEach( function(x) { assert(x.name != "test.test"); });

dd( "d" );
 
db.createCollection("test");
var found = false;
db.getCollection( "system.namespaces" ).find().forEach( function(x) {  if (x.name == "test.test") found = true; });
assert(found);

dd( "e" );

/*
 *  profile level
 */ 
 
db.setProfilingLevel(0);
assert(db.getProfilingLevel() == 0);

db.setProfilingLevel(1);
assert(db.getProfilingLevel() == 1);

db.setProfilingLevel(2);
assert(db.getProfilingLevel() == 2);

db.setProfilingLevel(0);
assert(db.getProfilingLevel() == 0);

dd( "f" );
asserted = false;
try {
    db.setProfilingLevel(10);
    assert(false);
}
catch (e) { 
    asserted = true;
    assert(e.dbSetProfilingException);
}
assert( asserted );

dd( "g" );

