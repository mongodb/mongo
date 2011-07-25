t = db.jstests_index9;

t.drop();
db.createCollection( "jstests_index9" );
assert.eq( 1, db.system.indexes.count( {ns: "test.jstests_index9"} ), "There should be 1 index with default collection" );
t.drop();
db.createCollection( "jstests_index9", {autoIndexId: true} );
assert.eq( 1, db.system.indexes.count( {ns: "test.jstests_index9"} ), "There should be 1 index if autoIndexId: true" );

t.drop();
db.createCollection( "jstests_index9", {autoIndexId:false} );
assert.eq( 0, db.system.indexes.count( {ns: "test.jstests_index9"} ), "There should be 0 index if autoIndexId: false" );
t.createIndex( { _id:1 } );
assert.eq( 1, db.system.indexes.count( {ns: "test.jstests_index9"} ) );
t.createIndex( { _id:1 } );
assert.eq( 1, db.system.indexes.count( {ns: "test.jstests_index9"} ) );

t.drop();
t.createIndex( { _id:1 } );
assert.eq( 1, db.system.indexes.count( {ns: "test.jstests_index9"} ) );

t.drop();
t.save( {a:1} );
t.createIndex( { _id:1 } );
assert.eq( 1, db.system.indexes.count( {ns: "test.jstests_index9"} ) );
