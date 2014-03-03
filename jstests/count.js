t = db.jstests_count;

t.drop();
t.save( { i: 1 } );
t.save( { i: 2 } );
assert.eq( 1, t.find( { i: 1 } ).count(), "A"  );
assert.eq( 1, t.count( { i: 1 } ) , "B" );
assert.eq( 2, t.find().count() , "C" );
assert.eq( 2, t.find( undefined ).count() , "D" );
assert.eq( 2, t.find( null ).count() , "E" );
assert.eq( 2, t.count() , "F" );

t.drop();
t.save( {a:true,b:false} );
t.ensureIndex( {b:1,a:1} );
assert.eq( 1, t.find( {a:true,b:false} ).count() , "G" );
assert.eq( 1, t.find( {b:false,a:true} ).count() , "H" );

t.drop();
t.save( {a:true,b:false} );
t.ensureIndex( {b:1,a:1,c:1} );

assert.eq( 1, t.find( {a:true,b:false} ).count() , "I" );
assert.eq( 1, t.find( {b:false,a:true} ).count() , "J" );

