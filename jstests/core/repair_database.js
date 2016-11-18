/**
 * This tests checks that repair database works and doesn't leave the database in a bad state
 * 1.) Drop "repairDB" database
 * 2.) Ensure repair works with single collection with an extra index. and one doc
 * 3.) Ensure repair works with no docs, same collection/index as above
 * 4.) Ensure repair works with 2 collections, one doc in each
 */

// 1. Drop db
var mydb = db.getSisterDB("repairDB");
mydb.dropDatabase();

var myColl = mydb.a;

// 2
var doc = {_id: 1, a: "hello world"};
myColl.insert(doc);
myColl.ensureIndex({a: 1});
mydb.repairDatabase();
var foundDoc = myColl.findOne();

assert.neq(null, foundDoc);
assert.eq(1, foundDoc._id);

assert.docEq(doc, myColl.findOne({a: doc.a}));
assert.docEq(doc, myColl.findOne({_id: 1}));

// 3
var myColl2 = mydb.b;
myColl.remove({});
mydb.repairDatabase();

// 4
var myColl2 = mydb.b;
myColl.insert(doc);
myColl2.insert(doc);
mydb.repairDatabase();
assert.docEq(doc, myColl.findOne({a: doc.a}));
assert.docEq(doc, myColl2.findOne({a: doc.a}));
