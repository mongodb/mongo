t = db.jstests_or7;
t.drop();

t.ensureIndex( {a:1} );
t.save( {a:2} );

assert.eq.automsg( "1", "t.count( {$or:[{a:{$in:[1,3]}},{a:2}]} )" );