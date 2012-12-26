load( "jstests/libs/fts.js" );

t = db.text_blog;
t.drop();

t.save( { _id : 1 , title : "my blog post" , text : "this is a new blog i am writing. yay" } );
t.save( { _id : 2 , title : "my 2nd post" , text : "this is a new blog i am writing. yay" } );
t.save( { _id : 3 , title : "knives are Fun" , text : "this is a new blog i am writing. yay" } );

// default weight is 1
// specify weights if you want a field to be more meaningull
t.ensureIndex( { "title" : "text" , text : "text" } , { weights : { title : 10 } } );

res = t.runCommand( "text" , { search : "blog" } )
assert.eq( 3, res.results.length );
assert.eq( 1, res.results[0].obj._id );

res = t.runCommand( "text" , { search : "write" } )
assert.eq( 3, res.results.length );
assert.eq( res.results[0].score, res.results[1].score );
assert.eq( res.results[0].score, res.results[2].score );







