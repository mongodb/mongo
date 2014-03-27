t = db.geo_polygon4;
t.drop();

shouldRun = true;

bi = db.adminCommand( "buildinfo" ).sysInfo
if ( bi.indexOf( "erh2" ) >= 0 ){
    // this machine runs this test very slowly
    // it seems to be related to osx 10.5
    // if this machine gets upgraded, we should remove this check
    // the os x debug builders still run thistest, so i'm not worried about it
    shouldRun = false;
}

if ( shouldRun ) {

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
    
        assert.between( 9 - 2 , t.find( { loc: { "$within": { "$polygon" : [[0,0], [1,1], [0,2]] }}} ).count() , 9, "Triangle Test", true);
        assert.eq( num , t.find( { loc : { "$within" : { "$polygon" : [ [-180,-180], [-180,180], [180,180], [180,-180] ] } } } ).count() , "Bounding Box Test" );
        
        assert.eq( 441 , t.find( { loc : { "$within" : { "$polygon" : [ [0,0], [0,10], [10,10], [10,0] ] } } } ).count() , "Square Test" );
        assert.eq( 25 , t.find( { loc : { "$within" : { "$polygon" : [ [0,0], [0,2], [2,2], [2,0] ] } } } ).count() , "Square Test 2" );
            
        if(1){ // SERVER-3726
        // Points exactly on diagonals may be in or out, depending on how the error calculating the slope falls.  
        assert.between( 341 - 18 , t.find( { loc : { "$within" : { "$polygon" : [ [0,0], [0,10], [10,10], [10,0], [5,5] ] } } } ).count(), 341, "Square Missing Chunk Test", true );
        assert.between( 21 - 2 , t.find( { loc : { "$within" : { "$polygon" : [ [0,0], [0,2], [2,2], [2,0], [1,1] ] } } } ).count(), 21 , "Square Missing Chunk Test 2", true );
        }
    
        assert.eq( 1 , t.find( { loc: { "$within": { "$polygon" : [[0,0], [0,0], [0,0]] }}} ).count() , "Point Test" );
        
        // SERVER-3725
        {
        assert.eq( 5 , t.find( { loc: { "$within": { "$polygon" : [[0,0], [1,0], [2,0]] }}} ).count() , "Line Test 1" );
        assert.eq( 3 , t.find( { loc: { "$within": { "$polygon" : [[0,0], [0,0], [1,0]] }}} ).count() , "Line Test 2" );
        assert.eq( 5 , t.find( { loc: { "$within": { "$polygon" : [[0,2], [0,1], [0,0]] }}} ).count() , "Line Test 3" );
        }
        
        assert.eq( 3 , t.find( { loc: { "$within": { "$polygon" : [[0,1], [0,0], [0,0]] }}} ).count() , "Line Test 4" );
    }
}
