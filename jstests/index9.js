t = db.jstests_index9;

t.drop();
db.createCollection( "jstests_index9", {autoIndexId:false} );
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
