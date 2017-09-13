db.jstests_capped.drop();
db.createCollection("jstests_capped", {capped: true, size: 30000});

t = db.jstests_capped;
assert.eq(1, t.getIndexes().length, "expected a count of one index for new capped collection");

t.save({x: 1});
t.save({x: 2});

assert(t.find().sort({$natural: 1})[0].x == 1, "expected obj.x==1");
assert(t.find().sort({$natural: -1})[0].x == 2, "expected obj.x == 2");
