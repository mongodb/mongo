
load( "jstests/libs/fts.js" );

t = db.text_neg;
t.drop();

t.save( { _id: 1, x: 'hello pig', y: 'pig oink' } )
t.save( { _id: 2, x: 'hello cat', y: 'cat meow' } )
t.save( { _id: 3, x: 'pig cat', y: 'bunny' } )

t.ensureIndex( { x : 'text', y : 'text' } )

assert.eq( [2], queryIDS( t , "hello -pig" ) , "-pig" );
assert.eq( [1,2], queryIDS( t , "pig cat -bunny" ) , "-bunny" );
assert.eq( [1], queryIDS( t , "hello -'cat meow'" ) , "-phrase" );
assert.eq( [], queryIDS( t , "hello -pig -cat" ) , "-all" );
