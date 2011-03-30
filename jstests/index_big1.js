
t = db.index_big1;

N = 2000;
t.drop();

for ( i=0; i<N; i++ ) {

    var s = "";
    while ( s.length < i )
        s += "x";

    t.insert( { a : i + .5 , x : s } )
}

t.ensureIndex( { a : 1 , x : 1 } )

assert.eq( 2 , t.getIndexes().length );

flip = -1;

for ( i=0; i<N; i++ ) {
    var c = t.find( { a : i + .5 } ).count();
    if ( c == 1 ) {
        assert.eq( -1 , flip , "flipping : " + i );
    }
    else {
        if ( flip == -1 ) {
            print( "flipped at: " + i );
            flip = i;
        }
    }
}

assert.eq( 798, flip , "flip changed" );
