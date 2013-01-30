
t = db.big_object1
t.drop();

if ( db.adminCommand( "buildinfo" ).bits == 64 ){
    
    var large = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    var s = large;
    while ( s.length < 850 * 1024 ){
        s += large;
    }
    x = 0;
    while ( true ){
        n = { _id : x , a : [] }
        for ( i=0; i<14+x; i++ )
            n.a.push( s )
        try {
            t.insert( n )
            o = n
        }
        catch ( e ){
            break;
        }
        
        if ( db.getLastError() != null )
            break;
        x++;
    }
    
    printjson( t.stats(1024*1024) )

    assert.lt( 15 * 1024 * 1024 , Object.bsonsize( o ) , "A1" )
    assert.gt( 17 * 1024 * 1024 , Object.bsonsize( o ) , "A2" )
    
    assert.eq( x , t.count() , "A3" )
    
    for ( i=0; i<x; i++ ){
        o = t.findOne( { _id : i } )
        try {
            // test large mongo -> js conversion
            var a = o.a;
        } catch(e) {
            assert(false, "Caught exception trying to insert during iteration " + i + ": " + e);
        }
        assert( o , "B" + i );
    }
    
    t.drop()
}
else {
    print( "skipping big_object1 b/c not 64-bit" )
}

print("SUCCESS");
