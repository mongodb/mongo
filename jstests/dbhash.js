
a = db.dbhasha;
b = db.dbhashb;

a.drop();
b.drop();

// debug SERVER-761
db.getCollectionNames().forEach( function( x ) { 
                                v = db[ x ].validate()
                                assert( v.valid, x + " " + tojson( v ) );
                                } );

function gh( coll , mydb ){
    if ( ! mydb ) mydb = db;
    var x = mydb.runCommand( "dbhash" ).collections[coll.getName()];
    if ( ! x )
        return "";
    return x;
}

function dbh( mydb ){
    return mydb.runCommand( "dbhash" ).md5;
}

assert.eq( gh( a ) , gh( b ) , "A1" );

a.insert( { _id : 5 } );
assert.neq( gh( a ) , gh( b ) , "A2" );

b.insert( { _id : 5 } );
assert.eq( gh( a ) , gh( b ) , "A3" );

dba = db.getSisterDB( "dbhasha" );
dbb = db.getSisterDB( "dbhashb" );

dba.dropDatabase();
dbb.dropDatabase();

assert.eq( gh( dba.foo , dba ) , gh( dbb.foo , dbb ) , "B1" );
assert.eq( dbh( dba ) , dbh( dbb ) , "C1" );

dba.foo.insert( { _id : 5 } );
assert.neq( gh( dba.foo , dba ) , gh( dbb.foo , dbb ) , "B2" );
assert.neq( dbh( dba ) , dbh( dbb ) , "C2" );

dbb.foo.insert( { _id : 5 } );
assert.eq( gh( dba.foo , dba ) , gh( dbb.foo , dbb ) , "B3" );
assert.eq( dbh( dba ) , dbh( dbb ) , "C3" );
