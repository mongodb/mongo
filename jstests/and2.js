// Test dollar sign operator with $and SERVER-1089

t = db.jstests_and2;

t.drop();
t.save( {a:[1,2]} );
t.update( {a:1}, {$set:{'a.$':5}} );
assert.eq( [5,2], t.findOne().a );

t.drop();
t.save( {a:[1,2]} );
t.update( {$and:[{a:1}]}, {$set:{'a.$':5}} );
assert.eq( [5,2], t.findOne().a );

// Make sure dollar sign operator with $and is consistent with no $and case
t.drop();
t.save( {a:[1,2],b:[3,4]} );
t.update( {a:1,b:4}, {$set:{'a.$':5}} );
// Probably not what we want here, just trying to make sure $and is consistent
assert.eq( {a:[1,5],b:[3,4]}, t.find( {}, {_id:0} ).toArray()[ 0 ] );

// Make sure dollar sign operator with $and is consistent with no $and case
t.drop();
t.save( {a:[1,2],b:[3,4]} );
t.update( {a:1,$and:[{b:4}]}, {$set:{'a.$':5}} );
// Probably not what we want here, just trying to make sure $and is consistent
assert.eq( {a:[1,5],b:[3,4]}, t.find( {}, {_id:0} ).toArray()[ 0 ] );
