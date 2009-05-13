t = db.jstests_count;

t.drop();
t.save( { i: 1 } );
t.save( { i: 2 } );
assert.eq( 1, t.find( { i: 1 } ).count() );
assert.eq( 1, t.count( { i: 1 } ) );
assert.eq( 2, t.find().count() );
assert.eq( 2, t.find( undefined ).count() );
assert.eq( 2, t.find( null ).count() );
assert.eq( 2, t.count() );

t.drop();
t.save( {a:true,b:false} );
t.ensureIndex( {b:1,a:1} );
assert.eq( 1, t.find( {a:true,b:false} ).count() );
assert.eq( 1, t.find( {b:false,a:true} ).count() );

//t.drop();
//t.save( {a:true,b:false} );
//t.ensureIndex( {b:1,a:1,c:1} );
//assert.eq( 1, t.find( {a:true,b:false} ).count() );
//assert.eq( 1, t.find( {b:false,a:true} ).count() );
