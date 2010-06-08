t = db.jstests_or6;
t.drop();

t.ensureIndex( {a:1} );

assert.eq.automsg( "2", "t.find( {$or:[{a:{$gt:2}},{a:{$gt:0}}]} ).explain().clauses[ 1 ].indexBounds[ 0 ][ 1 ].a" );
assert.eq.automsg( "2", "t.find( {$or:[{a:{$lt:2}},{a:{$lt:4}}]} ).explain().clauses[ 1 ].indexBounds[ 0 ][ 0 ].a" );

printjson( t.find( {$or:[{a:{$lt:2}},{a:{$lt:4}}]} ).explain() );