
t = db.index_diag
t.drop();

t.ensureIndex( { x : 1 } );

all = []
ids = []
xs = []

function r( a ){
    var n = []
    for ( var x=a.length-1; x>=0; x-- )
        n.push( a[x] );
    return n;
}

for ( i=1; i<4; i++ ){
    o = { _id : i , x : -i }
    t.insert( o );
    all.push( o );
    ids.push( { _id : i } );
    xs.push( { x : -i } );
}

assert.eq( all , t.find().sort( { _id : 1 } ).toArray() , "A1" );
assert.eq( r( all ) , t.find().sort( { _id : -1 } ).toArray() , "A2" );

assert.eq( all , t.find().sort( { x : -1 } ).toArray() , "A3" );
assert.eq( r( all ) , t.find().sort( { x : 1 } ).toArray() , "A4" );

assert.eq( ids , t.find().sort( { _id : 1 } )._addSpecial( "$returnKey" , true ).toArray() , "B1" )
assert.eq( r( ids ) , t.find().sort( { _id : -1 } )._addSpecial( "$returnKey" , true ).toArray() , "B2" )
assert.eq( xs , t.find().sort( { x : -1 } )._addSpecial( "$returnKey" , true ).toArray() , "B3" )
assert.eq( r( xs ) , t.find().sort( { x : 1 } )._addSpecial( "$returnKey" , true ).toArray() , "B4" )

assert.eq( r( xs ) , t.find().hint( { x : 1 } )._addSpecial( "$returnKey" , true ).toArray() , "B4" )

// SERVER-4981
t.ensureIndex( { _id : 1 , x : 1 } );
assert.eq( all ,
          t.find().hint( { _id : 1 , x : 1 } )._addSpecial( "$returnKey" , true ).toArray()
          )
assert.eq( r( all ) ,
          t.find().hint( { _id : 1 , x : 1 } ).sort( { x : 1 } )
          ._addSpecial( "$returnKey" , true ).toArray()
          )

// Descriptive test that a query involving multiple plans may return keys from multiple indexes.
t.dropIndex( { _id : 1 , x : 1 } );
ret =
t.find( { _id : { $gt : -10 }, x : { $gt : -10 } } )._addSpecial( "$returnKey" , true ).toArray()
keys = {};
for( i in ret ) {
    Object.extend( keys, ret[ i ] );
}
assert.eq( [ "_id" , "x" ], Object.keySet( keys ).sort() );

assert.eq( [ {} , {} , {} ],
          t.find().hint( { $natural : 1 } )._addSpecial( "$returnKey" , true ).toArray() )
