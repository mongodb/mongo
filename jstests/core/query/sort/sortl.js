// Tests equality query on _id with a sort, intended to be tested on both mongos and mongod. For
// SERVER-20641.
// @tags: [
//   # Metadata projection behavior differs between find and aggregate.
//   incompatible_with_views,
// ]
let coll = db.sortl;
coll.drop();

assert.commandWorked(coll.insert({_id: 1, a: 2}));
let res = coll.find({_id: 1}).sort({a: 1});
assert.eq(res.next(), {_id: 1, a: 2});
assert.eq(res.hasNext(), false);

res = coll.find({_id: 1}, {b: {$meta: "sortKey"}}).sort({a: 1});
assert.eq(res.next(), {_id: 1, a: 2, b: [2]});
assert.eq(res.hasNext(), false);

res = db.runCommand({
    findAndModify: coll.getName(),
    query: {_id: 1},
    update: {$set: {b: 1}},
    sort: {a: 1},
    fields: {c: {$meta: "sortKey"}},
});
assert.commandFailedWithCode(res, ErrorCodes.BadValue, "$meta sortKey update");

coll.drop();
assert.commandWorked(coll.insert({_id: 1, a: 2}));

res = db.runCommand({
    findAndModify: coll.getName(),
    query: {_id: 1},
    remove: true,
    sort: {b: 1},
    fields: {c: {$meta: "sortKey"}},
});
assert.commandFailedWithCode(res, ErrorCodes.BadValue, "$meta sortKey delete");

coll.drop();
