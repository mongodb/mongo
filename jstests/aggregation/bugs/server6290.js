// The $isoDate operator is not available.  SERVER-6290

t = db.jstests_aggregation_server6290;
t.drop();

t.save( {} );

function assertInvalidOperator( pipeline ) {
    assert.eq( 15999, // exception: invalid operator
               t.aggregate( pipeline ).code );
}

// $isoDate is an invalid operator.
assertInvalidOperator( { $project:{ a:{ $isoDate:[ { year:1 } ] } } } );
// $date is an invalid operator.
assertInvalidOperator( { $project:{ a:{ $date:[ { year:1 } ] } } } );

// Alternative operands.
assertInvalidOperator( { $project:{ a:{ $isoDate:[] } } } );
assertInvalidOperator( { $project:{ a:{ $date:[] } } } );
assertInvalidOperator( { $project:{ a:{ $isoDate:'foo' } } } );
assertInvalidOperator( { $project:{ a:{ $date:'foo' } } } );

// Test with $group.
assertInvalidOperator( { $group:{ _id:0, a:{ $first:{ $isoDate:[ { year:1 } ] } } } } );
assertInvalidOperator( { $group:{ _id:0, a:{ $first:{ $date:[ { year:1 } ] } } } } );
