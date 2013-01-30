load( "jstests/libs/fts.js" );

t = db.text_proj;
t.drop();

t.save( { _id : 1 , x : "a", y: "b", z : "c"});
t.save( { _id : 2 , x : "d", y: "e", z : "f"});
t.save( { _id : 3 , x : "a", y: "g", z : "h"});

t.ensureIndex( { x : "text"} , { default_language : "none" } );

res = t.runCommand("text", {search : "a"});
assert.eq( 2, res.results.length );
assert( res.results[0].obj.y, tojson(res) );

res = t.runCommand("text", {search : "a", project: {x: 1}});
assert.eq( 2, res.results.length );
assert( !res.results[0].obj.y, tojson(res) );




