// SERVER-2282 $or de duping with sparse indexes

t = db.jstests_org;
t.drop();

t.ensureIndex( {a:1}, {sparse:true} );
t.ensureIndex( {b:1} );

t.remove();
t.save( {a:1,b:2} );
assert.eq( 1, t.count( {$or:[{a:1},{b:2}]} ) );

t.remove();
t.save( {a:null,b:2} );
assert.eq( 1, t.count( {$or:[{a:null},{b:2}]} ) );

t.remove();
t.save( {b:2} );
assert.eq( 1, t.count( {$or:[{a:null},{b:2}]} ) );
