// SERVER-9385 ensure saving a document derived from bson->js conversion doesn't lose it's _id
//
// @tags: [requires_fastcount]

let t = db[jsTestName()];
t.drop();

t.insert({_id: 1, x: 1});
let x = t.findOne();
x._id = 2;
t.save(x);

t.find().forEach(printjson);

assert.eq(2, t.find().count());
assert.eq(2, t.find().itcount());

assert(t.findOne({_id: 1}), "original insert missing");
assert(t.findOne({_id: 2}), "save didn't work?");
