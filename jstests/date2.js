// Check that it's possible to compare a Date to a Timestamp - SERVER-3304
// Check Date / Timestamp comparison equivalence - SERVER-3222

t = db.jstests_date2;
t.drop();

t.ensureIndex( {a:1} );

t.save( {a:new Timestamp()} );

if ( 0 ) { // SERVER-3304
assert.eq( 1, t.find( {a:{$gt:new Date(0)}} ).itcount() );
}
