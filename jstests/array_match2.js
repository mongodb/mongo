// Different recursive array match cases SERVER-2898

t = db.jstests_array_match2;
t.drop();

t.save( {a:[{1:4},5]} );
// When the array index is the last field, both of these match types work.
assert.eq( 1, t.count( {'a.1':4} ) );
assert.eq( 1, t.count( {'a.1':5} ) );

t.remove();
// When the array index is not the last field, only one of the match types works.
t.save( {a:[{1:{foo:4}},{foo:5}]} );
if ( 0 ) { // SERVER-2898
assert.eq( 1, t.count( {'a.1.foo':4} ) );
}
assert.eq( 1, t.count( {'a.1.foo':5} ) );

// Same issue with the $exists operator
t.remove();
t.save( {a:[{1:{foo:4}},{}]} );
assert.eq( 1, t.count( {'a.1':{$exists:true}} ) );
if ( 0 ) { // SERVER-2898
assert.eq( 1, t.count( {'a.1.foo':{$exists:true}} ) );
}
