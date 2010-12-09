
t = db.big_object1
t.drop();

s = ""
while ( s.length < 512 * 1024 ){
    s += "x";
}

x = 0;
while ( true ){
    o = { _id : x , a : [] }
    for ( i=0; i<x; i++ )
        o.a.push( s )
    print( Object.bsonsize( o ) )
    try {
        t.insert( o )
    }
    catch ( e ){
        break;
    }
    
    if ( db.getLastError() != null )
        break;
    x++;
}

assert.lt( 15 * 1024 * 1024 , Object.bsonsize( o ) , "A1" )
assert.gt( 17 * 1024 * 1024 , Object.bsonsize( o ) , "A2" )

assert.eq( x , t.count() , "A3" )

for ( i=0; i<x; i++ ){
    o = t.findOne( { _id : 1 } )
    assert( o , "B" + i );
}

t.drop()
