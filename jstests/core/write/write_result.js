// @tags: [
//   # {multi: true} update testing behavior not applicable to sharded clusters.
//   assumes_unsharded_collection,
//   assumes_write_concern_unchanged,
//   requires_multi_updates,
//   requires_non_retryable_writes,
//   requires_fastcount,
// ]

//
// Tests the behavior of single writes using write commands
//

let coll = db.write_result;
coll.drop();

let result = null;

//
// Basic insert
coll.remove({});
printjson((result = coll.insert({foo: "bar"})));
assert.eq(result.nInserted, 1);
assert.eq(result.nUpserted, 0);
assert.eq(result.nMatched, 0);
assert.eq(result.nModified, 0);
assert.eq(result.nRemoved, 0);
assert(!result.getWriteError());
assert(!result.getWriteConcernError());
assert(!result.getUpsertedId());
assert.eq(coll.count(), 1);

//
// Basic upsert (using save)
coll.remove({});
var id = new ObjectId();
printjson((result = coll.save({_id: id, foo: "bar"})));
assert.eq(result.nInserted, 0);
assert.eq(result.nUpserted, 1);
assert.eq(result.nMatched, 0);
assert.eq(result.nModified, 0);
assert.eq(result.nRemoved, 0);
assert(!result.getWriteError());
assert(!result.getWriteConcernError());
assert.eq(result.getUpsertedId()._id, id);
assert.eq(coll.count(), 1);

//
// Basic update
coll.remove({});
coll.insert({foo: "bar"});
printjson((result = coll.update({foo: "bar"}, {$set: {foo: "baz"}})));
assert.eq(result.nInserted, 0);
assert.eq(result.nUpserted, 0);
assert.eq(result.nMatched, 1);
assert.eq(result.nModified, 1);
assert.eq(result.nRemoved, 0);
assert(!result.getWriteError());
assert(!result.getWriteConcernError());
assert(!result.getUpsertedId());
assert.eq(coll.count(), 1);

//
// Basic multi-update
coll.remove({});
coll.insert({foo: "bar"});
coll.insert({foo: "bar", set: ["value"]});
printjson((result = coll.update({foo: "bar"}, {$addToSet: {set: "value"}}, {multi: true})));
assert.eq(result.nInserted, 0);
assert.eq(result.nUpserted, 0);
assert.eq(result.nMatched, 2);
assert.eq(result.nModified, 1);
assert.eq(result.nRemoved, 0);
assert(!result.getWriteError());
assert(!result.getWriteConcernError());
assert(!result.getUpsertedId());
assert.eq(coll.count(), 2);

//
// Basic remove
coll.remove({});
coll.insert({foo: "bar"});
printjson((result = coll.remove({})));
assert.eq(result.nInserted, 0);
assert.eq(result.nUpserted, 0);
assert.eq(result.nMatched, 0);
assert.eq(result.nModified, 0);
assert.eq(result.nRemoved, 1);
assert(!result.getWriteError());
assert(!result.getWriteConcernError());
assert(!result.getUpsertedId());
assert.eq(coll.count(), 0);

//
// Insert with error
coll.remove({});
var id = new ObjectId();
coll.insert({_id: id, foo: "bar"});
printjson((result = coll.insert({_id: id, foo: "baz"})));
assert.eq(result.nInserted, 0);
assert(result.getWriteError());
assert(result.getWriteError().errmsg);
assert(!result.getWriteConcernError());
assert.eq(coll.count(), 1);

//
// Update with error
coll.remove({});
coll.insert({foo: "bar"});
result = coll.update({foo: "bar"}, {_id: /a/});
assert.eq(result.nUpserted, 0);
assert.eq(result.nMatched, 0);
assert.eq(0, result.nModified, tojson(result));
assert(result.getWriteError());
assert(result.getWriteError().errmsg);
assert(!result.getUpsertedId());
assert.eq(coll.count(), 1);

//
// Multi-update with error
coll.remove({});
var id = new ObjectId();
for (let i = 0; i < 10; ++i) coll.insert({value: NumberInt(i)});
coll.insert({value: "not a number"});
// $bit operator fails when the field is not integer
// Note that multi-updates do not currently report partial stats if they fail
printjson((result = coll.update({}, {$bit: {value: {and: NumberInt(0)}}}, {multi: true})));
assert.eq(result.nUpserted, 0);
assert.eq(result.nMatched, 0);
assert.eq(0, result.nModified, tojson(result));
assert(result.getWriteError());
assert(result.getWriteError().errmsg);
assert(!result.getUpsertedId());
assert.eq(coll.count(), 11);

//
// Bulk insert
coll.remove({});
printjson((result = coll.insert([{foo: "bar"}, {foo: "baz"}])));
assert.eq(result.nInserted, 2);
assert(!result.hasWriteErrors());
assert(!result.hasWriteConcernError());
assert.eq(coll.count(), 2);

//
// Bulk insert with error
coll.remove({});
var id = new ObjectId();
// Second insert fails with duplicate _id
printjson(
    (result = coll.insert([
        {_id: id, foo: "bar"},
        {_id: id, foo: "baz"},
    ])),
);
assert.eq(result.nInserted, 1);
assert(result.hasWriteErrors());
assert(!result.hasWriteConcernError());
assert.eq(coll.count(), 1);

//
// Custom write concern
// (More detailed write concern tests require custom/replicated servers)
coll.remove({});
coll.setWriteConcern({w: "majority"});
printjson((result = coll.insert({foo: "bar"})));
assert.eq(result.nInserted, 1);
assert(!result.getWriteError());
assert(!result.getWriteConcernError());
assert.eq(coll.count(), 1);
coll.unsetWriteConcern();

//
// Write concern error
// NOTE: In a replica set write concern is checked after write
coll.remove({});
let wRes = assert.writeError(coll.insert({foo: "bar"}, {writeConcern: {w: "invalid"}}));
let res = assert.commandWorked(db.hello());
let replSet = res.hasOwnProperty("$clusterTime");
if (!replSet) assert.eq(coll.count(), 0, "not replset");
else assert.eq(coll.count(), 1, "replset");
