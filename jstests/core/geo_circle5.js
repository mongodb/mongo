// reported as server-1238.

db.server1238.drop();
db.server1238.remove({});
db.server1238.save({loc: [5000000, 900000], id: 1});
db.server1238.save({loc: [5000000, 900000], id: 2});
db.server1238.ensureIndex({loc: "2d"}, {min: -21000000, max: 21000000});
db.server1238.save({loc: [5000000, 900000], id: 3});
db.server1238.save({loc: [5000000, 900000], id: 4});

c1 = db.server1238.find({"loc": {"$within": {"$center": [[5000000, 900000], 1.0]}}}).count();

c2 = db.server1238.find({"loc": {"$within": {"$center": [[5000001, 900000], 5.0]}}}).count();

assert.eq(4, c1, "A1");
assert.eq(c1, c2, "B1");
// print(db.server1238.find({"loc" : {"$within" : {"$center" : [[5000001, 900000],
// 5.0]}}}).toArray());
// [
// {
// "_id" : ObjectId("4c173306f5d9d34a46cb7b11"),
// "loc" : [
// 5000000,
// 900000
// ],
// "id" : 4
// }
// ]
