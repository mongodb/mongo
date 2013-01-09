// test collection
tc = db.text_enable_flag;
tc.drop();

// text search disabled => ensureIndex returns error
db.adminCommand( { setParameter : "*", textSearchEnabled : false } );
res = tc.ensureIndex( { "field": "text" } );
assert.neq( res, undefined );

// text search enabled => ensureIndex succeeds (returns nothing)
db.adminCommand( { setParameter : "*", textSearchEnabled : true } );
res = tc.ensureIndex( { "field": "text" } );
assert.eq( res, undefined );
