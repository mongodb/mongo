
t = db.queryoptimizer1;
t.drop();

for ( i=0; i<1000; i++ )
    for ( j=0; j<20; j++ )
        t.save( { a : i , b : i , c : j } )


t.ensureIndex( { a : 1 } )
t.ensureIndex( { b : 1 } )

for ( ; i<2000; i++ )
    for ( j=0; j<20; j++ )
        t.save( { a : i , b : i , c : j } )


printjson( t.find( { a : 50 , b : 50 , c : 6 } ).explain() );

for ( var i=0; i<10000; i++ ){
    a = t.find( { a : 50 , b : 50 , c : i % 20 } ).toArray();
}

printjson( t.find( { a : 50 , b : 50 , c : 6 } ).explain() );
assert.eq( 1 , t.find( { a : 50 , b : 50 , c : 6 } ).count() );

t.drop();