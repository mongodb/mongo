// tests to make sure that the new _id is returned after the insert
t = db.upsert1;
t.drop();

// make sure the new _id is returned when $mods are used
t.update( { x : 1 } , { $inc : { y : 1 } } , true );
l = db.getLastErrorCmd();
assert( l.upserted , "A1 - " + tojson(l) );
assert.eq( l.upserted.str , t.findOne()._id.str , "A2" );

// make sure the new _id is returned on a replacement (no $mod in update)
t.update( { x : 2 } , { x : 2 , y : 3 } , true );
l = db.getLastErrorCmd();
assert( l.upserted , "B1 - " + tojson(l) );
assert.eq( l.upserted.str , t.findOne( { x : 2 } )._id.str , "B2" );
assert.eq( 2 , t.find().count() , "B3" );

// use the _id from the query for the insert
t.update({_id:3}, {$set: {a:'123'}}, true)
l = db.getLastErrorCmd();
assert( l.upserted , "C1 - " + tojson(l) );
assert.eq( l.upserted , 3 , "C2 - " + tojson(l) );

// test with an embedded doc for the _id field
t.update({_id:{a:1}}, {$set: {a:123}}, true)
l = db.getLastErrorCmd();
assert( l.upserted , "D1 - " + tojson(l) );
assert.eq( l.upserted , {a:1} , "D2 - " + tojson(l) );

// test with a range query
t.update({_id: {$gt:100}}, {$set: {a:123}}, true)
l = db.getLastErrorCmd();
assert( l.upserted , "E1 - " + tojson(l) );
assert.neq( l.upserted , 100 , "E2 - " + tojson(l) );

// test with an _id query
t.update({_id: 1233}, {$set: {a:123}}, true)
l = db.getLastErrorCmd();
assert( l.upserted , "F1 - " + tojson(l) );
assert.eq( l.upserted , 1233 , "F2 - " + tojson(l) );

// test with an embedded _id query
t.update({_id: {a:1, b:2}}, {$set: {a:123}}, true)
l = db.getLastErrorCmd();
assert( l.upserted , "G1 - " + tojson(l) );
assert.eq( l.upserted , {a:1, b:2} , "G2 - " + tojson(l) );

// test with no _id inserted
db.no_id.drop();
db.createCollection("no_id", {autoIndexId:false})
db.no_id.update({foo:1}, {$set:{a:1}}, true)
l = db.getLastErrorCmd();
assert.eq( l.upserted , undefined , "H1 - " + tojson(l) );
assert.eq( 0, db.no_id.getIndexes().length, "H2" );
assert.eq( 1, db.no_id.count(), "H3" );
assert.eq( { foo : 1, a : 1 }, db.no_id.findOne(), "H4" );
