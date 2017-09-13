/**
 * Tests the resetError logic when the bulk api enforces the write concern for unordered
 * writes. The tests indirectly checks whether resetError was called by inspecting the
 * response of the getLastError command after executing the bulk ops.
 */

var coll = db.bulk_legacy_enforce_gle;

// batch of size 1 no error case.
coll.drop();
var bulk = coll.initializeUnorderedBulkOp();
bulk.find({none: 1}).upsert().updateOne({_id: 1});
assert(bulk.execute() instanceof BulkWriteResult);

var gle = db.runCommand({getLastError: 1});
assert(gle.ok, tojson(gle));
assert.eq(1, gle.n);

// batch of size 1 should not call resetError even when it errors out.
coll.drop();
coll.insert({_id: 1});
bulk = coll.initializeUnorderedBulkOp();
bulk.find({none: 1}).upsert().updateOne({_id: 1});
assert.throws(function() {
    bulk.execute();
});

gle = db.runCommand({getLastError: 1});
assert(gle.ok, tojson(gle));
assert.neq(null, gle.err);

// batch with all error except last should not call resetError.
coll.drop();
coll.insert({_id: 1});
bulk = coll.initializeUnorderedBulkOp();
bulk.find({none: 1}).upsert().updateOne({_id: 1});
bulk.find({none: 1}).upsert().updateOne({_id: 1});
bulk.find({none: 1}).upsert().updateOne({_id: 0});
var res = assert.throws(function() {
    bulk.execute();
});
assert.eq(2, res.getWriteErrors().length);

gle = db.runCommand({getLastError: 1});
assert(gle.ok, tojson(gle));
assert.eq(1, gle.n);

// batch with error at middle should not call resetError.
coll.drop();
coll.insert({_id: 1});
bulk = coll.initializeUnorderedBulkOp();
bulk.find({none: 1}).upsert().updateOne({_id: 0});
bulk.find({none: 1}).upsert().updateOne({_id: 1});
bulk.find({none: 1}).upsert().updateOne({_id: 2});
var res = assert.throws(function() {
    bulk.execute();
});
assert.eq(1, res.getWriteErrors().length);

gle = db.runCommand({getLastError: 1});
assert(gle.ok, tojson(gle));
// mongos sends the bulk as one while the shell sends the write individually
assert.gte(gle.n, 1);

// batch with error at last should call resetError.
coll.drop();
coll.insert({_id: 2});
bulk = coll.initializeUnorderedBulkOp();
bulk.find({none: 1}).upsert().updateOne({_id: 0});
bulk.find({none: 1}).upsert().updateOne({_id: 1});
bulk.find({none: 1}).upsert().updateOne({_id: 2});
res = assert.throws(function() {
    bulk.execute();
});
assert.eq(1, res.getWriteErrors().length);

gle = db.runCommand({getLastError: 1});
assert(gle.ok, tojson(gle));
assert.eq(0, gle.n);

// batch with error at last should not call resetError if { w: 1 }
coll.drop();
coll.insert({_id: 2});
bulk = coll.initializeUnorderedBulkOp();
bulk.find({none: 1}).upsert().updateOne({_id: 0});
bulk.find({none: 1}).upsert().updateOne({_id: 1});
bulk.find({none: 1}).upsert().updateOne({_id: 2});
res = assert.throws(function() {
    bulk.execute();
});
assert.eq(1, res.getWriteErrors().length);

gle = db.runCommand({getLastError: 1, w: 1});
assert(gle.ok, tojson(gle));
assert.neq(null, gle.err);

// batch with error at last should not call resetError if { w: 0 }
coll.drop();
coll.insert({_id: 2});
bulk = coll.initializeUnorderedBulkOp();
bulk.find({none: 1}).upsert().updateOne({_id: 0});
bulk.find({none: 1}).upsert().updateOne({_id: 1});
bulk.find({none: 1}).upsert().updateOne({_id: 2});
res = assert.throws(function() {
    bulk.execute();
});
assert.eq(1, res.getWriteErrors().length);

gle = db.runCommand({getLastError: 1, w: 0});
assert(gle.ok, tojson(gle));
assert.neq(null, gle.err);
