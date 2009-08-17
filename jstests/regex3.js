
t = db.regex3;
t.drop();

t.save( { name : "eliot" } );
t.save( { name : "emily" } );
t.save( { name : "bob" } );
t.save( { name : "aaron" } );

assert.eq( 2 , t.find( { name : /^e.*/ } ).count() , "no index count" );
assert.eq( 4 , t.find( { name : /^e.*/ } ).explain().nscanned , "no index explain" );
t.ensureIndex( { name : 1 } );
assert.eq( 2 , t.find( { name : /^e.*/ } ).count() , "index count" );
assert.eq( 2 , t.find( { name : /^e.*/ } ).explain().nscanned , "index explain" ); // SERVER-239
