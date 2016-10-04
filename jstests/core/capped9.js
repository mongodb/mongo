
t = db.capped9;
t.drop();

db.createCollection("capped9", {capped: true, size: 1024 * 50});

t.insert({_id: 1, x: 2, y: 3});

assert.eq(1, t.find({x: 2}).itcount(), "A1");
assert.eq(1, t.find({y: 3}).itcount(), "A2");
// assert.throws( function(){ t.find( { _id : 1 } ).itcount(); } , [] , "A3" ); // SERVER-3064

t.update({_id: 1}, {$set: {y: 4}});
// assert( db.getLastError() , "B1" ); // SERVER-3064
// assert.eq( 3 , t.findOne().y , "B2" ); // SERVER-3064

t.ensureIndex({_id: 1});

assert.eq(1, t.find({_id: 1}).itcount(), "D1");

assert.writeOK(t.update({_id: 1}, {$set: {y: 4}}));
assert.eq(4, t.findOne().y, "D2");
