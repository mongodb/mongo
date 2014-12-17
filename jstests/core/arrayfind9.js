// Assorted $elemMatch behavior checks.

t = db.jstests_arrayfind9;
t.drop();

// Top level field $elemMatch:$not matching
t.save( { a:[ 1 ] } );
assert.eq( 1, t.count( { a:{ $elemMatch:{ $not:{ $ne:1 } } } } ) );

// Top level field object $elemMatch matching.
t.drop();
t.save( { a:[ {} ] } );
assert.eq( 1, t.count( { a:{ $elemMatch:{ $gte:{} } } } ) );

// Top level field array $elemMatch matching.
t.drop();
t.save( { a:[ [] ] } );
assert.eq( 1, t.count( { a:{ $elemMatch:{ $in:[ [] ] } } } ) );

// Matching by array index.
t.drop();
t.save( { a:[ [ 'x' ] ] } );
assert.eq( 1, t.count( { a:{ $elemMatch:{ '0':'x' } } } ) );

// Matching multiple values of a nested array.
t.drop();
t.save( { a:[ { b:[ 0, 2 ] } ] } );
t.ensureIndex( { a:1 } );
t.ensureIndex( { 'a.b':1 } );
plans = [ { $natural:1 }, { a:1 }, { 'a.b':1 } ];
for( i in plans ) {
    p = plans[ i ];
    assert.eq( 1, t.find( { a:{ $elemMatch:{ b:{ $gte:1, $lte:1 } } } } ).hint( p ).itcount() );
}
