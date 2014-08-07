/**
 * This test checks update bulk api corner cases for single nodes:
 *   -- upsert no _id
 *   -- other?
 */

var mongod24 = MongoRunner.runMongod({ binVersion : "2.4" });

var coll24 = mongod24.getCollection("foo.bar");

// 2.4 mongod fluent api

var upsertedId = ObjectId().toString();
var bulk = coll24.initializeOrderedBulkOp();
bulk.find({ _id : upsertedId }).upsert().update({ $set : { a : 1 } });
var res = bulk.execute();
printjson(res);
assert.eq(1, res.nUpserted);
assert.eq(upsertedId, res.getUpsertedIdAt(0)._id);

jsTest.log("DONE!");

MongoRunner.stopMongod(mongod24);


