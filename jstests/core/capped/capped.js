/**
 * @tags: [
 *   requires_capped,
 *   # capped collections connot be sharded
 *   assumes_unsharded_collection,
 * ]
 */

db[jsTestName()].drop();
db.createCollection(jsTestName(), {capped: true, size: 30000});

let t = db[jsTestName()];
assert.eq(1, t.getIndexes().length, "expected a count of one index for new capped collection");

t.save({x: 1});
t.save({x: 2});

assert(t.find().sort({$natural: 1})[0].x == 1, "expected obj.x==1");
assert(t.find().sort({$natural: -1})[0].x == 2, "expected obj.x == 2");
