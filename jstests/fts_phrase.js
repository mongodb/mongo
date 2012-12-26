
load( "jstests/libs/fts.js" );

t = db.text_phrase;
t.drop()

t.save( { _id : 1 , title : "my blog post" , text : "i am writing a blog. yay" } );
t.save( { _id : 2 , title : "my 2nd post" , text : "this is a new blog i am typing. yay" } );
t.save( { _id : 3 , title : "knives are Fun" , text : "this is a new blog i am writing. yay" } );

t.ensureIndex( { "title" : "text" , text : "text" } , { weights : { title : 10 } } );

res = t.runCommand( "text" , { search : "blog write" } );
assert.eq( 3, res.results.length );
assert.eq( 1, res.results[0].obj._id );
assert( res.results[0].score > (res.results[1].score*2), tojson(res) );

res = t.runCommand( "text" , { search : "write blog" } );
assert.eq( 3, res.results.length );
assert.eq( 1, res.results[0].obj._id );
assert( res.results[0].score > (res.results[1].score*2), tojson(res) );






