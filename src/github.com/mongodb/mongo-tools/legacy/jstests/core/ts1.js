t = db.ts1
t.drop()

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

assert.eq( N , t.find( { x : { $type : 17 } } ).itcount() , "B1" )
assert.eq( 0 , t.find( { x : { $type : 3 } } ).itcount() , "B2" )

t.insert( { _id : 100 , x : new Timestamp( 123456 , 50 ) } )
x = t.findOne( { _id : 100 } ).x
assert.eq( 123456 , x.t , "C1" )
assert.eq( 50 , x.i , "C2" )

