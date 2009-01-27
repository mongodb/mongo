/**
 *   Tests for the db object enhancement
 */

dd = function( x ){
    //print( x );
}

dd( "a" );

db = connect( "test" )

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

/*
  * dbEval tested via collections count function
  */

/*
  * db group
  */

db.getCollection( "test" ).drop();
db.getCollection( "test" ).save({a:1});
db.getCollection( "test" ).save({a:1});

var f = db.group(
    {
        ns: "test",
        key: { a:true},
        cond: { a:1 },
        reduce: function(obj,prev) { prev.csum++; } ,
        initial: { csum: 0}
    }
);

assert(f[0].a == 1 && f[0].csum == 2);  
