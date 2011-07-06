
t = db.index_maxkey;

for ( var indexVersion=0; indexVersion<=1; indexVersion++ ) {
    t.drop();
    
    s = "";

    t.ensureIndex( { s : 1 } , { v : indexVersion } );
    while ( true ) {
        t.insert( { s : s } );
        if ( t.find().count() == t.find().sort( { s : 1 } ).itcount() ) {
            s += ".....";
            continue;
        }
        var sz = Object.bsonsize( { s : s } ) - 2;
        print( "indexVersion: " + indexVersion + " max key is : " + sz );
        if ( indexVersion == 0 ) {
            assert.eq( 821 , sz ); 
        }
        else if ( indexVersion == 1 ) { 
            assert.eq( 1026 , sz );
        }
        break;
    }
    
}
