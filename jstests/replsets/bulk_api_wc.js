// Tests write-concern-related bulk api functionality
(function() {

jsTest.log("Starting bulk api write concern tests...");

// Skip this test when running with storage engines other than inMemory, as the test relies on
// journaling not being active.
if (jsTest.options().storageEngine !== "inMemory") {
    jsTest.log("Skipping test because it is only applicable for the inMemory storage engine");
    return;
}

// Start a 2-node replica set with no journal.
// Allows testing immediate write concern failures and wc application failures
var rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
var mongod = rst.getPrimary();
var coll = mongod.getCollection("test.bulk_api_wc");

// Create a unique index, legacy writes validate too early to use invalid documents for write error
// testing
coll.createIndex({a: 1}, {unique: true});

//
// Ordered
//

//
// Fail due to nojournal
coll.remove({});
var bulk = coll.initializeOrderedBulkOp();
bulk.insert({a: 1});
bulk.insert({a: 2});
assert.throws(function() {
    bulk.execute({j: true});
});

//
// Fail due to unrecognized write concern field.
coll.remove({});
var bulk = coll.initializeOrderedBulkOp();
bulk.insert({a: 1});
bulk.insert({a: 2});
assert.throws(function() {
    bulk.execute({x: 1});
});

//
// Fail with write error, no write concern error even though it would fail on apply for ordered
coll.remove({});
var bulk = coll.initializeOrderedBulkOp();
bulk.insert({a: 1});
bulk.insert({a: 2});
bulk.insert({a: 2});
result = assert.throws(function() {
    bulk.execute({w: 'invalid'});
});
assert.eq(result.nInserted, 2);
assert.eq(result.getWriteErrors()[0].index, 2);
assert(!result.getWriteConcernError());
assert.eq(coll.find().itcount(), 2);

//
// Unordered
//

//
// Fail with write error, write concern error reported when unordered
coll.remove({});
var bulk = coll.initializeUnorderedBulkOp();
bulk.insert({a: 1});
bulk.insert({a: 2});
bulk.insert({a: 2});
var result = assert.throws(function() {
    bulk.execute({w: 'invalid'});
});
assert.eq(result.nInserted, 2);
assert.eq(result.getWriteErrors()[0].index, 2);
assert(result.getWriteConcernError());
assert.eq(coll.find().itcount(), 2);

//
// Fail with write error, write concern timeout reported when unordered Note that wtimeout:true can
// only be reported when the batch is all the same, so there's not multiple wc errors
coll.remove({});
var bulk = coll.initializeUnorderedBulkOp();
bulk.insert({a: 1});
bulk.insert({a: 2});
bulk.insert({a: 2});
var result = assert.throws(function() {
    bulk.execute({w: 3, wtimeout: 1});
});
assert.eq(result.nInserted, 2);
assert.eq(result.getWriteErrors()[0].index, 2);
assert.eq(100, result.getWriteConcernError().code);
assert.eq(coll.find().itcount(), 2);

//
// Fail with write error and upserted, write concern error reported when unordered
coll.remove({});
var bulk = coll.initializeUnorderedBulkOp();
bulk.insert({a: 1});
bulk.insert({a: 2});
bulk.find({a: 3}).upsert().updateOne({a: 3});
bulk.insert({a: 3});
var result = assert.throws(function() {
    bulk.execute({w: 'invalid'});
});
assert.eq(result.nInserted, 2);
assert.eq(result.nUpserted, 1);
assert.eq(result.getUpsertedIds()[0].index, 2);
assert.eq(result.getWriteErrors()[0].index, 3);
assert(result.getWriteConcernError());
assert.eq(coll.find().itcount(), 3);

jsTest.log("DONE bulk api wc tests");
rst.stopSet();
})();
