
t = db.regex4;
t.drop();

t.save( { name : "eliot" } );
t.save( { name : "emily" } );
t.save( { name : "bob" } );
t.save( { name : "aaron" } );

assert.eq( 2 , t.find( { name : /^e.*/ } ).count() , "no index count" );
assert.eq( 4 , t.find( { name : /^e.*/ } ).explain().nscanned , "no index explain" );
//assert.eq( 2 , t.find( { name : { $ne : /^e.*/ } } ).count() , "no index count ne" ); // SERVER-251

t.ensureIndex( { name : 1 } );

assert.eq( 2 , t.find( { name : /^e.*/ } ).count() , "index count" );
assert.eq( 2 , t.find( { name : /^e.*/ } ).explain().nscanned , "index explain" ); // SERVER-239
//assert.eq( 2 , t.find( { name : { $ne : /^e.*/ } } ).count() , "index count ne" ); // SERVER-251
