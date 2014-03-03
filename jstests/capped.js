db.jstests_capped.drop();
db.createCollection("jstests_capped", {capped:true, size:30000});

assert.eq( 1, db.system.indexes.find( {ns:"test.jstests_capped"} ).count(), "expected a count of one index for new capped collection" );
t = db.jstests_capped;

t.save({x:1});
t.save({x:2});

assert( t.find().sort({$natural:1})[0].x == 1 , "expected obj.x==1");
assert( t.find().sort({$natural:-1})[0].x == 2, "expected obj.x == 2");
