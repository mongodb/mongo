
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

t.drop();

t.save( { name : "aa" } );
t.save( { name : "ab" } );
t.save( { name : "ac" } );
t.save( { name : "c" } );

assert.eq( 3 , t.find( { name : /^aa*/ } ).count() , "B ni" );
t.ensureIndex( { name : 1 } );
assert.eq( 3 , t.find( { name : /^aa*/ } ).count() , "B i 1" );
assert.eq( 4 , t.find( { name : /^aa*/ } ).explain().nscanned , "B i 1 e" );

assert.eq( 2 , t.find( { name : /^a[ab]/ } ).count() , "B i 2" );
assert.eq( 2 , t.find( { name : /^a[bc]/ } ).count() , "B i 3" );

t.drop();

t.save( { name: "" } );
assert.eq( 1, t.count( { name: /^a?/ } ) , "C 1" );
t.ensureIndex( { name: 1 } );
assert.eq( 1, t.count( { name: /^a?/ } ) , "C 2");
