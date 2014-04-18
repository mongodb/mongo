//
//  $push acquired the possibility of sorting the resulting array as part of SERVER-8008. This
//  test exercises such $sort clause from the shell user's perspective.
//

t = db.push_sort;
t.drop();

//
// Valid Cases
//

// $slice amount is too large to kick in.
t.save( { _id: 1, x: [ {a:1}, {a:2} ] } );
t.update( {_id:1}, { $push: { x: { $each: [ {a:3} ], $slice:-5, $sort: {a:1} } } } )
assert.eq( [{a:1}, {a:2}, {a:3}] , t.findOne( {_id:1} ).x );

// $slice amount kicks in using values of both the base doc and of the $each clause.
t.save({ _id: 2, x: [ {a:1}, {a:3} ] } );
t.update( {_id:2}, { $push: { x: { $each: [ {a:2} ], $slice:-2, $sort: {a:1} } } } )
assert.eq( [{a:2}, {a:3}], t.findOne( {_id:2} ).x );

// $sort is descending and $slice is too large to kick in.
t.save({ _id: 3, x: [ {a:1}, {a:3} ] } );
t.update( {_id:3}, { $push: { x: { $each: [ {a:2} ], $slice:-5, $sort: {a:-1} } } } )
assert.eq( [{a:3}, {a:2}, {a:1}], t.findOne( {_id:3} ).x );

// $sort is descending and $slice kicks in using values of both the base doc and of
// the $each clause.
t.save({ _id: 4, x: [ {a:1}, {a:3} ] } );
t.update( {_id:4}, { $push: { x: { $each: [ {a:2} ], $slice:-2, $sort: {a:-1} } } } )
assert.eq( [{a:2}, {a:1}], t.findOne( {_id:4} ).x );

// $sort over only a portion of the array's elements objects and #slice kicking in
// using values of both the base doc and of the $each clause.
t.save({ _id: 5, x: [ {a:1,b:2}, {a:3,b:1} ] } );
t.update( {_id:5}, { $push: { x: { $each: [ {a:2,b:3} ], $slice:-2, $sort: {b:1} } } } )
assert.eq( [{a:1, b:2}, {a:2,b:3}], t.findOne( {_id:5} ).x );

// $sort over an array of nested objects and $slice too large to kick in.
t.save({ _id: 6, x: [ {a:{b:2}}, {a:{b:1}} ] } );
t.update( {_id:6}, { $push: { x: { $each: [ {a:{b:3}} ], $slice:-5, $sort: {'a.b':1} } } } )
assert.eq( [{a:{b:1}}, {a:{b:2}}, {a:{b:3}}], t.findOne( {_id:6} ).x );

// $sort over an array of nested objects and $slice kicking in using values of both the
// base doc and of the $each clause.
t.save({ _id: 7, x: [ {a:{b:2}}, {a:{b:1}} ] } );
t.update( {_id:7}, { $push: { x: { $each: [ {a:{b:3}} ], $slice:-2, $sort: {'a.b':1} } } } )
assert.eq( [{a:{b:2}}, {a:{b:3}}], t.findOne( {_id:7} ).x );

//
// Invalid Cases
//

t.save({ _id: 100, x: [ {a:1} ] } );

// For now, elements of the $each vector need to be objects. In here, '2' is an invalide $each.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [ 2 ], $slice:-2, $sort:{a:1} } } } ) )

// For the same reason as above, '1' is an invalid $each element.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2},1], $slice:-2, $sort:{a:1} } } }))

// The sort key pattern cannot be empty.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $slice:-2, $sort:{} } } } ) )

// For now, we do not support positive $slice's (ie, trimming from the array's front).
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $slice:2, $sort: {a:1} } } }))

// A $slice cannot be a fractional value.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $slice:-2.1, $sort: {a:1} } }}))

// The sort key pattern's value must be either 1 or -1. In here, {a:-2} is an invalid value.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $slice:-2, $sort: {a:-2} } } }))

// For now, we are not supporting sorting of basic elements (non-object, non-arrays). In here,
// the $sort clause would need to have a key pattern value rather than 1.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $slice:-2, $sort: 1 } } } ) )

// The key pattern 'a.' is an invalid value for $sort.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $slice:-2, $sort: {'a.':1} }}}))

// An empty key pattern is not a valid $sort value.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $slice:-2, $sort: {'':1} } } }))

// If a $slice is used, the only other $sort clause that's accepted is $sort. In here, $xxx
// is not a valid clause.
assert.throws( t.update( {_id:100}, { $push: { x: { $each: [{a:2}], $slice:-2, $xxx: {s:1} } } } ) )

t.remove({})

// Ensure that existing values are validated in the array as objects during a $sort with $each,
// not only the elements in the $each array.
t.save({ _id: 100, x: [ 1, "foo" ] } );
assert.throws(t.update(
    {_id: 100},
    { $push: { x: { $each: [{a:2}], $slice:-2, $sort: {a:1} } } } ) )
