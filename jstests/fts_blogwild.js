load( "jstests/libs/fts.js" );

t = db.text_blogwild;
t.drop();

t.save( { _id: 1 , title: "my blog post" , text: "this is a new blog i am writing. yay eliot" } );
t.save( { _id: 2 , title: "my 2nd post" , text: "this is a new blog i am writing. yay" } );
t.save( { _id: 3 , title: "knives are Fun for writing eliot" , text: "this is a new blog i am writing. yay" } );

// default weight is 1
// specify weights if you want a field to be more meaningull
t.ensureIndex( { dummy: "text" } , { weights: "$**" } );

res = t.runCommand( "text" , { search: "blog" } );
assert.eq( 3 , res.stats.n , "A1" );

res = t.runCommand( "text" , { search: "write" } );
assert.eq( 3 , res.stats.n , "B1" );

// mixing
t.dropIndex( "dummy_text" );
assert.eq( 1 , t.getIndexKeys().length , "C1" );
t.ensureIndex( { dummy: "text" } , { weights: { "$**": 1 , title: 2 } } );


res = t.runCommand( "text" , { search: "write" } );
assert.eq( 3 , res.stats.n , "C2" );
assert.eq( 3 , res.results[0].obj._id , "C3" );

res = t.runCommand( "text" , { search: "blog" } );
assert.eq( 3 , res.stats.n , "D1" );
assert.eq( 1 , res.results[0].obj._id , "D2" );

res = t.runCommand( "text" , { search: "eliot" } );
assert.eq( 2 , res.stats.n , "E1" );
assert.eq( 3 , res.results[0].obj._id , "E2" );






