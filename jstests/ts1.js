t = db.ts1

N = 20

for ( i=0; i<N; i++ ){
    t.insert( { _id : i , x : new Timestamp() } )
    sleep( 100 )
}

function get(i){
    return t.findOne( { _id : i } ).x;
}

function cmp( a , b ){
    if ( a.t < b.t )
        return -1;
    if ( a.t > b.t )
        return 1;
    
    return a.i - b.i;
}

for ( i=0; i<N-1; i++ ){
    a = get(i);
    b = get(i+1);
    //print( tojson(a) + "\t" + tojson(b) + "\t" + cmp(a,b) );
    assert.gt( 0 , cmp( a , b ) , "cmp " + i  )
}
