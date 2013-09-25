
t = db.index_check6;
t.drop();

t.ensureIndex( { age : 1 , rating : 1 } );

for ( var age=10; age<50; age++ ){
    for ( var rating=0; rating<10; rating++ ){
        t.save( { age : age , rating : rating } );
    }
}

assert.eq( 10 , t.find( { age : 30 } ).itcount());
// NEW QUERY EXPLAIN
assert.eq( 20 , t.find( { age : { $gte : 29 , $lte : 30 } } ).itcount());
// NEW QUERY EXPLAIN
assert.eq( 12 , t.find( { age : { $gte : 25 , $lte : 30 }, rating: {$in: [0,9] } } ).hint( {age:1,rating:1} ).itcount());
// NEW QUERY EXPLAIN
assert.eq( 12 , t.find( { age : { $gte : 25 , $lte : 30 }, rating: {$in: [0,8] } } ).hint( {age:1,rating:1} ).itcount());
// NEW QUERY EXPLAIN
assert.eq( 12 , t.find( { age : { $gte : 25 , $lte : 30 }, rating: {$in: [1,8] } } ).hint( {age:1,rating:1} ).itcount());

// NEW QUERY EXPLAIN
assert.eq( 2 , t.find( { age : { $gte : 29 , $lte : 30 } , rating : 5 } ).hint( {age:1,rating:1} ).itcount()); // SERVER-371
// NEW QUERY EXPLAIN
assert.eq( 4 , t.find( { age : { $gte : 29 , $lte : 30 } , rating : { $gte : 4 , $lte : 5 } } ).hint( {age:1,rating:1} ).itcount()); // SERVER-371

/* NEW QUERY EXPLAIN
assert.eq.automsg( "2", "t.find( { age:30, rating:{ $gte:4, $lte:5} } ).explain().nscanned" );
*/

t.drop();

for ( var a=1; a<10; a++ ){
    for ( var b=0; b<10; b++ ){
        for ( var c=0; c<10; c++ ) {
            t.save( { a:a, b:b, c:c } );
        }
    }
}

function doQuery( count, query, sort, index ) {
    // NEW QUERY EXPLAIN
    assert.eq( count, t.find( query ).hint( index ).sort( sort ).itcount() );
}

function doTest( sort, index ) {
    doQuery( 1, { a:5, b:5, c:5 }, sort, index );
    doQuery( 2, { a:5, b:5, c:{$gte:5,$lte:6} }, sort, index );
    doQuery( 1, { a:5, b:5, c:{$gte:5.5,$lte:6} }, sort, index );
    doQuery( 1, { a:5, b:5, c:{$gte:5,$lte:5.5} }, sort, index );
    doQuery( 3, { a:5, b:5, c:{$gte:5,$lte:7} }, sort, index );
    // NEW QUERY EXPLAIN
    doQuery( 2, { a:5, b:{$gte:5,$lte:6}, c:5 }, sort, index );
    if ( sort.b > 0 ) {
        // NEW QUERY EXPLAIN
        doQuery( 1, { a:5, b:{$gte:5.5,$lte:6}, c:5 }, sort, index );
        // NEW QUERY EXPLAIN
        doQuery( 1, { a:5, b:{$gte:5,$lte:5.5}, c:5 }, sort, index );
    } else {
        // NEW QUERY EXPLAIN
        doQuery( 1, { a:5, b:{$gte:5.5,$lte:6}, c:5 }, sort, index );
        // NEW QUERY EXPLAIN
        doQuery( 1, { a:5, b:{$gte:5,$lte:5.5}, c:5 }, sort, index );
    }
    // NEW QUERY EXPLAIN
    doQuery( 3, { a:5, b:{$gte:5,$lte:7}, c:5 }, sort, index );
    // NEW QUERY EXPLAIN
    doQuery( 2, { a:{$gte:5,$lte:6}, b:5, c:5 }, sort, index );    
    if ( sort.a > 0 ) {
        // NEW QUERY EXPLAIN
        doQuery( 1, { a:{$gte:5.5,$lte:6}, b:5, c:5 }, sort, index );
        // NEW QUERY EXPLAIN
        doQuery( 1, { a:{$gte:5,$lte:5.5}, b:5, c:5 }, sort, index );    
        // NEW QUERY EXPLAIN
        doQuery( 2, { a:{$gte:5.5,$lte:6}, b:5, c:{$gte:5,$lte:6} }, sort, index );    
    } else {
        // NEW QUERY EXPLAIN
        doQuery( 1, { a:{$gte:5.5,$lte:6}, b:5, c:5 }, sort, index );
        // NEW QUERY EXPLAIN
        doQuery( 1, { a:{$gte:5,$lte:5.5}, b:5, c:5 }, sort, index );    
        // NEW QUERY EXPLAIN
        doQuery( 2, { a:{$gte:5.5,$lte:6}, b:5, c:{$gte:5,$lte:6} }, sort, index );    
    }
    // NEW QUERY EXPLAIN
    doQuery( 3, { a:{$gte:5,$lte:7}, b:5, c:5 }, sort, index );
    // NEW QUERY EXPLAIN
    doQuery( 4, { a:{$gte:5,$lte:6}, b:5, c:{$gte:5,$lte:6} }, sort, index );    
    // NEW QUERY EXPLAIN
    doQuery( 4, { a:5, b:{$gte:5,$lte:6}, c:{$gte:5,$lte:6} }, sort, index );    
    // NEW QUERY EXPLAIN
    doQuery( 4, { a:{$gte:5,$lte:6}, b:{$gte:5,$lte:6}, c:5 }, sort, index );    
    // NEW QUERY EXPLAIN
    doQuery( 8, { a:{$gte:5,$lte:6}, b:{$gte:5,$lte:6}, c:{$gte:5,$lte:6} }, sort, index );    
}

for ( var a = -1; a <= 1; a += 2 ) {
    for( var b = -1; b <= 1; b += 2 ) {
        for( var c = -1; c <= 1; c += 2 ) {
            t.dropIndexes();
            var spec = {a:a,b:b,c:c};
            t.ensureIndex( spec );
            doTest( spec, spec );
            doTest( {a:-a,b:-b,c:-c}, spec );
        }
    }
}

