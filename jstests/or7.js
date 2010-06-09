t = db.jstests_or7;
t.drop();

t.ensureIndex( {a:1} );
t.save( {a:2} );

assert.eq.automsg( "1", "t.count( {$or:[{a:{$in:[1,3]}},{a:2}]} )" );

t.remove();

t.save( {a:"aa"} );
t.save( {a:"ac"} );

//SERVER-1201
if ( 0 ) {
printjson( t.find( {$or:[{a:/^ab/},{a:/^a/}]} ).explain() );
assert.eq.automsg( "2", "t.count( {$or:[{a:/^ab/},{a:/^a/}]} )" );
}

