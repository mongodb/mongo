// SERVER-7343: allow $within without a geo index.
t = db.geo_box1_noindex;
t.drop();

num = 0;
for ( x=0; x<=20; x++ ){
    for ( y=0; y<=20; y++ ){
        o = { _id : num++ , loc : [ x , y ] }
        t.save( o )
    }
}

searches = [
    [ [ 1 , 2 ] , [ 4 , 5 ] ] ,
    [ [ 1 , 1 ] , [ 2 , 2 ] ] ,
    [ [ 0 , 2 ] , [ 4 , 5 ] ] ,
    [ [ 1 , 1 ] , [ 2 , 8 ] ] ,
];

for ( i=0; i<searches.length; i++ ){
    b = searches[i];
    q = { loc : { $within : { $box : b } } }
    numWanted = ( 1 + b[1][0] - b[0][0] ) * ( 1 + b[1][1] - b[0][1] );
    assert.eq( numWanted  , t.find(q).itcount() , "itcount: " + tojson( q ) );
    printjson( t.find(q).explain() )
}

assert.eq( 0 , t.find( { loc : { $within : { $box : [ [100 , 100 ] , [ 110 , 110 ] ] } } } ).itcount() , "E1" )
assert.eq( 0 , t.find( { loc : { $within : { $box : [ [100 , 100 ] , [ 110 , 110 ] ] } } } ).count() , "E2" )
assert.eq( num , t.find( { loc : { $within : { $box : [ [ 0 , 0 ] , [ 110 , 110 ] ] } } } ).count() , "E3" )
assert.eq( num , t.find( { loc : { $within : { $box : [ [ 0 , 0 ] , [ 110 , 110 ] ] } } } ).itcount() , "E4" )
assert.eq( 57 , t.find( { loc : { $within : { $box : [ [ 0 , 0 ] , [ 110 , 110 ] ] } } } ).limit(57).itcount() , "E5" )
