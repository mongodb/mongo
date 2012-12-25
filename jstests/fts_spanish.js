
load( "jstests/libs/fts.js" );

t = db.text_spanish;
t.drop();

t.save( { _id: 1, title: "mi blog", text: "Este es un blog de prueba" } );
t.save( { _id: 2, title: "mi segundo post", text: "Este es un blog de prueba" } );
t.save( { _id: 3, title: "cuchillos son divertidos", text: "este es mi tercer blog stemmed" } );
t.save( { _id: 4, language: "english", title: "My fourth blog", text: "This stemmed blog is in english" } );

// default weight is 1
// specify weights if you want a field to be more meaningull
t.ensureIndex( { "title": "text", text: "text" }, { weights: { title: 10 },
                                                        default_language: "spanish" } );

res = t.runCommand( "text", { search: "blog" } );
assert.eq( 4, res.results.length );

assert.eq( [4], queryIDS( t, "stem" ) );
assert.eq( [3], queryIDS( t, "stemmed" ) );
assert.eq( [4], queryIDS( t, "stemmed", null, { language : "english" } ) );

assert.eq( [1,2], queryIDS( t, "prueba" ) );








