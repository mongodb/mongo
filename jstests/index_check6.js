
t = db.index_check6;
t.drop();

t.ensureIndex( { age : 1 , rating : 1 } );

for ( var age=10; age<50; age++ ){
    for ( var rating=0; rating<10; rating++ ){
        t.save( { age : age , rating : rating } );
    }
}

assert.eq( 10 , t.find( { age : 30 } ).explain().nscanned , "A" );
assert.eq( 20 , t.find( { age : { $gte : 29 , $lte : 30 } } ).explain().nscanned , "B" );
assert.eq( 18 , t.find( { age : { $gte : 25 , $lte : 30 }, rating: {$in: [0,9] } } ).explain().nscanned , "C1" );
assert.eq( 23 , t.find( { age : { $gte : 25 , $lte : 30 }, rating: {$in: [0,8] } } ).explain().nscanned , "C2" );
assert.eq( 28 , t.find( { age : { $gte : 25 , $lte : 30 }, rating: {$in: [1,8] } } ).explain().nscanned , "C3" );

assert.eq( 4 , t.find( { age : { $gte : 29 , $lte : 30 } , rating : 5 } ).explain().nscanned , "C" ); // SERVER-371
assert.eq( 6 , t.find( { age : { $gte : 29 , $lte : 30 } , rating : { $gte : 4 , $lte : 5 } } ).explain().nscanned , "D" ); // SERVER-371

assert.eq.automsg( "2", "t.find( { age:30, rating:{ $gte:4, $lte:5} } ).explain().nscanned" );

t.drop();

for ( var a=1; a<10; a++ ){
    for ( var b=0; b<10; b++ ){
        for ( var c=0; c<10; c++ ) {
            t.save( { a:a, b:b, c:c } );
        }
    }
}

function doTest( s ) {
    sort = s;
assert.eq.automsg( "1", "t.find( { a:5, b:5, c:5 } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "2", "t.find( { a:5, b:5, c:{$gte:5,$lte:6} } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "1", "t.find( { a:5, b:5, c:{$gte:5.5,$lte:6} } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "1", "t.find( { a:5, b:5, c:{$gte:5,$lte:5.5} } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "3", "t.find( { a:5, b:5, c:{$gte:5,$lte:7} } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "4", "t.find( { a:5, b:{$gte:5,$lte:6}, c:5 } ).sort( sort ).explain().nscanned" );
    if ( s.b > 0 ) {
        assert.eq.automsg( "2", "t.find( { a:5, b:{$gte:5.5,$lte:6}, c:5 } ).sort( sort ).explain().nscanned" );
        assert.eq.automsg( "2", "t.find( { a:5, b:{$gte:5,$lte:5.5}, c:5 } ).sort( sort ).explain().nscanned" );        
    } else {
        assert.eq.automsg( "2", "t.find( { a:5, b:{$gte:5.5,$lte:6}, c:5 } ).sort( sort ).explain().nscanned" );
        assert.eq.automsg( "2", "t.find( { a:5, b:{$gte:5,$lte:5.5}, c:5 } ).sort( sort ).explain().nscanned" );   
    }
assert.eq.automsg( "7", "t.find( { a:5, b:{$gte:5,$lte:7}, c:5 } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "4", "t.find( { a:{$gte:5,$lte:6}, b:5, c:5 } ).sort( sort ).explain().nscanned" );
    if ( s.a > 0 ) {
        assert.eq.automsg( "2", "t.find( { a:{$gte:5.5,$lte:6}, b:5, c:5 } ).sort( sort ).explain().nscanned" );
        assert.eq.automsg( "2", "t.find( { a:{$gte:5,$lte:5.5}, b:5, c:5 } ).sort( sort ).explain().nscanned" );
        assert.eq.automsg( "3", "t.find( { a:{$gte:5.5,$lte:6}, b:5, c:{$gte:5,$lte:6} } ).sort( sort ).explain().nscanned" );
    } else {
        assert.eq.automsg( "2", "t.find( { a:{$gte:5.5,$lte:6}, b:5, c:5 } ).sort( sort ).explain().nscanned" );
        assert.eq.automsg( "2", "t.find( { a:{$gte:5,$lte:5.5}, b:5, c:5 } ).sort( sort ).explain().nscanned" );        
        assert.eq.automsg( "3", "t.find( { a:{$gte:5.5,$lte:6}, b:5, c:{$gte:5,$lte:6} } ).sort( sort ).explain().nscanned" );
    }
assert.eq.automsg( "7", "t.find( { a:{$gte:5,$lte:7}, b:5, c:5 } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "6", "t.find( { a:{$gte:5,$lte:6}, b:5, c:{$gte:5,$lte:6} } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "6", "t.find( { a:5, b:{$gte:5,$lte:6}, c:{$gte:5,$lte:6} } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "10", "t.find( { a:{$gte:5,$lte:6}, b:{$gte:5,$lte:6}, c:5 } ).sort( sort ).explain().nscanned" );
assert.eq.automsg( "14", "t.find( { a:{$gte:5,$lte:6}, b:{$gte:5,$lte:6}, c:{$gte:5,$lte:6} } ).sort( sort ).explain().nscanned" );
}

for ( var a = -1; a <= 1; a += 2 ) {
    for( var b = -1; b <= 1; b += 2 ) {
        for( var c = -1; c <= 1; c += 2 ) {
            t.dropIndexes();
            var spec = {a:a,b:b,c:c};
            t.ensureIndex( spec );
            doTest( spec );
            doTest( {a:-a,b:-b,c:-c} );
        }
    }
}

