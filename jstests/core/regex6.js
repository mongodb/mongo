// contributed by Andrew Kempe
t = db.regex6;
t.drop();

t.save( { name : "eliot" } );
t.save( { name : "emily" } );
t.save( { name : "bob" } );
t.save( { name : "aaron" } );
t.save( { name : "[with]some?symbols" } );

t.ensureIndex( { name : 1 } );

assert.eq( 0 , t.find( { name : /^\// } ).count() , "index count" );
assert.eq( 1 , t.find( { name : /^\// } ).explain().nscanned , "index explain 1" );
assert.eq( 0 , t.find( { name : /^é/ } ).explain().nscanned , "index explain 2" );
assert.eq( 0 , t.find( { name : /^\é/ } ).explain().nscanned , "index explain 3" );
assert.eq( 1 , t.find( { name : /^\./ } ).explain().nscanned , "index explain 4" );
assert.eq( 5 , t.find( { name : /^./ } ).explain().nscanned , "index explain 5" );

// SERVER-2862
assert.eq( 0 , t.find( { name : /^\Qblah\E/ } ).count() , "index explain 6" );
assert.eq( 1 , t.find( { name : /^\Qblah\E/ } ).explain().nscanned , "index explain 6" );
assert.eq( 1 , t.find( { name : /^blah/ } ).explain().nscanned , "index explain 6" );
assert.eq( 1 , t.find( { name : /^\Q[\Ewi\Qth]some?s\Eym/ } ).count() , "index explain 6" );
assert.eq( 2 , t.find( { name : /^\Q[\Ewi\Qth]some?s\Eym/ } ).explain().nscanned , "index explain 6" );
assert.eq( 2 , t.find( { name : /^bob/ } ).explain().nscanned , "index explain 6" ); // proof nscanned == count+1

assert.eq( 1, t.find( { name : { $regex : "^e", $gte: "emily" } } ).explain().nscanned , "ie7" );
assert.eq( 1, t.find( { name : { $gt : "a", $regex: "^emily" } } ).explain().nscanned , "ie7" );
