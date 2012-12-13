// Check $first/$last group accumulators.  SERVER-3862
// $first/$last select first/last value for a group key from the previous pipeline.

t = db.jstests_aggregation_firstlast;
t.drop();

/** Check expected $first and $last result values. */
function assertFirstLast( expectedFirst, expectedLast, pipeline, expression ) {
    pipeline = pipeline || [];
    expression = expression || '$b'
    pipeline.push( { $group:{ _id:'$a',
                              first:{ $first:expression },
                              last:{ $last:expression } } } );
    result = t.aggregate( pipeline ).result;
    for( var i = 0; i < result.length; ++i ) {
        if ( result[ i ]._id == 1 ) {
            // Check results for group _id 1.
            assert.eq( expectedFirst, result[ i ].first );
            assert.eq( expectedLast, result[ i ].last );
            return;
        }
    }
    assert( false, "Expected group _id '1' missing." );
}

// One document.
t.save( { a:1, b:1 } );
assertFirstLast( 1, 1 );

// Two documents.
t.save( { a:1, b:2 } );
assertFirstLast( 1, 2 );

// Three documents.
t.save( { a:1, b:3 } );
assertFirstLast( 1, 3 );

// Another 'a' key value does not affect outcome.
t.drop();
t.save( { a:3, b:0 } );
t.save( { a:1, b:1 } );
t.save( { a:1, b:2 } );
t.save( { a:1, b:3 } );
t.save( { a:2, b:0 } );
assertFirstLast( 1, 3 );

// Additional pipeline stages do not affect outcome if order is maintained.
assertFirstLast( 1, 3, [ { $project:{ x:'$a', y:'$b' } }, { $project:{ a:'$x', b:'$y' } } ] );

// Additional pipeline stages affect outcome if order is modified.
assertFirstLast( 3, 1, [ { $sort:{ b:-1 } } ] );

// Skip and limit affect the results seen.
t.drop();
t.save( { a:1, b:1 } );
t.save( { a:1, b:2 } );
t.save( { a:1, b:3 } );
assertFirstLast( 1, 2, [ { $limit:2 } ] );
assertFirstLast( 2, 3, [ { $skip:1 }, { $limit:2 } ] );
assertFirstLast( 2, 2, [ { $skip:1 }, { $limit:1 } ] );

// Mixed type values.
t.save( { a:1, b:'foo' } );
assertFirstLast( 1, 'foo' );

t.drop();
t.save( { a:1, b:'bar' } );
t.save( { a:1, b:true } );
assertFirstLast( 'bar', true );

// Value null.
t.drop();
t.save( { a:1, b:null } );
t.save( { a:1, b:2 } );
assertFirstLast( null, 2 );
t.drop();
t.save( { a:1, b:2 } );
t.save( { a:1, b:null } );
assertFirstLast( 2, null );
t.drop();
t.save( { a:1, b:null } );
t.save( { a:1, b:null } );
assertFirstLast( null, null );

// Value missing.
t.drop();
t.save( { a:1 } );
t.save( { a:1, b:2 } );
assertFirstLast( undefined, 2 );
t.drop();
t.save( { a:1, b:2 } );
t.save( { a:1 } );
assertFirstLast( 2, undefined );
t.drop();
t.save( { a:1 } );
t.save( { a:1 } );
assertFirstLast( undefined, undefined );

// Dotted field.
t.drop();
t.save( { a:1, b:[ { c:1 }, { c:2 } ] } );
t.save( { a:1, b:[ { c:6 }, {} ] } );
assertFirstLast( [ 1, 2 ], [ 6 ], [], '$b.c' );

// Computed expressions.
t.drop();
t.save( { a:1, b:1 } );
t.save( { a:1, b:2 } );
assertFirstLast( 1, 0, [], { $mod:[ '$b', 2 ] } );
assertFirstLast( 0, 1, [], { $mod:[ { $add:[ '$b', 1 ] }, 2 ] } );
