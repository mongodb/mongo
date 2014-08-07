
t = db.explain2
t.drop();

t.ensureIndex( { a : 1 , b : 1 } );

for ( i=1; i<10; i++ ){
    t.insert( { _id : i , a : i , b : i , c : i } );
}

function go( q , c , b , o ){
    var e = t.find( q ).hint( {a:1,b:1} ).explain();
    assert.eq( c , e.n , "count " + tojson( q ) )
    assert.eq( b , e.nscanned , "nscanned " + tojson( q ) )
    assert.eq( o , e.nscannedObjects , "nscannedObjects " + tojson( q ) )
}

q = { a : { $gt : 3 } }
go( q , 6 , 6 , 6 );

q.b = 5
go( q , 1 , 6 , 1 );

delete q.b
q.c = 5
go( q , 1 , 6 , 6 );

