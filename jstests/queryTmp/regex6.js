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
// NEW QUERY EXPLAIN
assert.eq( 0 , t.find( { name : /^\// } ).itcount());
assert.eq( 0 , t.find( { name : /^é/ } ).itcount());
assert.eq( 0 , t.find( { name : /^\é/ } ).itcount());
assert.eq( 0 , t.find( { name : /^\./ } ).itcount());
assert.eq( 5 , t.find( { name : /^./ } ).itcount());

// SERVER-2862
assert.eq( 0 , t.find( { name : /^\Qblah\E/ } ).count() , "index explain 6" );
/* NEW QUERY EXPLAIN
assert.eq( 1 , t.find( { name : /^\Qblah\E/ } ).explain().nscanned , "index explain 6" );
*/
assert.eq( 0 , t.find( { name : /^blah/ } ).itcount());
assert.eq( 1 , t.find( { name : /^\Q[\Ewi\Qth]some?s\Eym/ } ).count() , "index explain 6" );
/* NEW QUERY EXPLAIN
assert.eq( 2 , t.find( { name : /^\Q[\Ewi\Qth]some?s\Eym/ } ).explain().nscanned , "index explain 6" );
*/
assert.eq( 1 , t.find( { name : /^bob/ } ).itcount());

assert.eq( 1, t.find( { name : { $regex : "^e", $gte: "emily" } } ).itcount());
assert.eq( 1, t.find( { name : { $gt : "a", $regex: "^emily" } } ).itcount());
