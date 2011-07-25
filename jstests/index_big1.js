// check where "key to big" happens

t = db.index_big1;

N = 3200;
t.drop();

var s = "";

for ( i=0; i<N; i++ ) {

    t.insert( { a : i + .5 , x : s } )

    s += "x";
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
	    //            print( "state flipped at: " + i );
            flip = i;
        }
    }
}

//print(flip);
//print(flip/1024);

assert.eq( /*v0 index : 797*/1002, flip , "flip changed" );
