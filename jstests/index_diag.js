
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
    ids.push( { "" : i } );
    xs.push( { "" : -i } );
}

assert.eq( all , t.find().sort( { _id : 1 } ).toArray() , "A1" );
assert.eq( r( all ) , t.find().sort( { _id : -1 } ).toArray() , "A2" );

assert.eq( all , t.find().sort( { x : -1 } ).toArray() , "A3" );
assert.eq( r( all ) , t.find().sort( { x : 1 } ).toArray() , "A4" );

assert.eq( ids , t.find().sort( { _id : 1 } )._addSpecial( "$returnKey" , true ).toArray() , "B1" )
assert.eq( r( ids ) , t.find().sort( { _id : -1 } )._addSpecial( "$returnKey" , true ).toArray() , "B2" )
assert.eq( xs , t.find().sort( { x : -1 } )._addSpecial( "$returnKey" , true ).toArray() , "B3" )
assert.eq( r( xs ) , t.find().sort( {x : 1 } )._addSpecial( "$returnKey" , true ).toArray() , "B4" )

assert.eq( r( xs ) , t.find().hint( { x : 1 } )._addSpecial( "$returnKey" , true ).toArray() , "B4" )

