
t = db.index_covered1;
t.drop();

for ( i=1; i<10; i++ ){
    t.insert( { _id : i , a : i , b : i , c : i } );
}

t.ensureIndex( { a : 1 , b : 1 } );

/**
* c | count
* b | btree scanned
* o | objects loaded
*/
function go( q , f , c , b , o ){
    if ( f )
        f._id = true;

    var debug = tojson( q ) + "\t" + tojson( f );
    
    var e = t.find( q , f ).explain();
    assert.eq( c , e.n , "count " + debug )
    assert.eq( b , e.nscanned , "nscanned " + debug )
    assert.eq( o , e.nscannedObjects , "nscannedObjects " + debug )

    t.find( q , f ).forEach(
        function(z){
            if ( f ){
                var x = f.keySet();
                var y = z.keySet();
                Array.sort( x );
                Array.sort( y );
                assert.eq( x , y , "keys " + debug );
            }
        }
    );
}

q = { a : { $gt : 3 } }
go( q , null , 6 , 7 , 6 );
q.b = 5
go( q , null , 1 , 6 , 1 );
delete q.b
q.c = 5
go( q , null , 1 , 7 , 6 );


q = { a : { $gt : 3 } }
f = { _id : true }
//go( q , f , 6 , 7 , 0 );

