t = db.geo_polygon4;
t.drop();

num = 0;
for ( x = -180; x < 180; x += .5 ){
    for ( y = -180; y < 180; y += .5 ){
        o = { _id : num++ , loc : [ x , y ] };
        t.save( o );
    }
}

var numTests = 31;
for( var n = 0; n < numTests; n++ ){
    t.dropIndexes()
    t.ensureIndex( { loc : "2d" }, { bits : 2 + n } );


    assert.eq( 9 , t.find( { loc: { "$within": { "$polygon" : [[0,0], [1,1], [0,2]] }}} ).count() , "Triangle Test" );
    assert.eq( num , t.find( { loc : { "$within" : { "$polygon" : [ [-180,-180], [-180,180], [180,180], [180,-180] ] } } } ).count() , "Bounding Box Test" );

    assert.eq( 441 , t.find( { loc : { "$within" : { "$polygon" : [ [0,0], [0,10], [10,10], [10,0] ] } } } ).count() , "Square Test" );
    assert.eq( 25 , t.find( { loc : { "$within" : { "$polygon" : [ [0,0], [0,2], [2,2], [2,0] ] } } } ).count() , "Square Test 2" );
    if(0){ // SERVER-3726
    assert.eq( 331 , t.find( { loc : { "$within" : { "$polygon" : [ [0,0], [0,10], [10,10], [10,0], [5,5] ] } } } ).count() , "Square Missing Chunk Test" );
    assert.eq( 21 , t.find( { loc : { "$within" : { "$polygon" : [ [0,0], [0,2], [2,2], [2,0], [1,1] ] } } } ).count() , "Square Missing Chunk Test 2" );
    }

    assert.eq( 1 , t.find( { loc: { "$within": { "$polygon" : [[0,0], [0,0], [0,0]] }}} ).count() , "Point Test" );
    if(0){ // SERVER-3725
    assert.eq( 5 , t.find( { loc: { "$within": { "$polygon" : [[0,0], [1,0], [2,0]] }}} ).count() , "Line Test 1" );
    assert.eq( 3 , t.find( { loc: { "$within": { "$polygon" : [[0,0], [0,0], [1,0]] }}} ).count() , "Line Test 2" );
    assert.eq( 5 , t.find( { loc: { "$within": { "$polygon" : [[0,2], [0,1], [0,0]] }}} ).count() , "Line Test 3" );
    }
    assert.eq( 3 , t.find( { loc: { "$within": { "$polygon" : [[0,1], [0,0], [0,0]] }}} ).count() , "Line Test 4" );
}
