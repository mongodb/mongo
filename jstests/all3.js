// Check that $all matching null is consistent with $in - SERVER-3820

if ( 0 ) { // SERVER-3820

t = db.jstests_all3;
t.drop();

t.save({});

assert.eq( 1, t.count( {foo:{$in:[null]}} ) );
assert.eq( 1, t.count( {foo:{$all:[null]}} ) );
assert.eq( 0, t.count( {foo:{$not:{$all:[null]}}} ) );
assert.eq( 0, t.count( {foo:{$not:{$in:[null]}}} ) );

t.remove();
t.save({foo:1});
assert.eq( 0, t.count( {foo:{$in:[null]}} ) );
assert.eq( 0, t.count( {foo:{$all:[null]}} ) );
assert.eq( 1, t.count( {foo:{$not:{$in:[null]}}} ) );
assert.eq( 1, t.count( {foo:{$not:{$all:[null]}}} ) );

t.remove();
t.save( {foo:[0,1]} );
assert.eq( 1, t.count( {foo:{$in:[[0,1]]}} ) );
assert.eq( 1, t.count( {foo:{$all:[[0,1]]}} ) );

t.remove();
t.save( {foo:[]} );
assert.eq( 1, t.count( {foo:{$in:[[]]}} ) );
assert.eq( 1, t.count( {foo:{$all:[[]]}} ) );

}