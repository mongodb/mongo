/**
 * Tests the basic behaviours and properties of single-field hashed indexes.
 * Cannot implicitly shard accessed collections because of extra shard key index in sharded
 * collection.
 * @tags: [
 *   assumes_no_implicit_index_creation,
 *   requires_fastcount,
 * ]
 */
import {getWinningPlan, isIxscan} from "jstests/libs/analyze_plan.js";

const t = db.single_field_hashed_index;
t.drop();

assert.commandWorked(db.createCollection(t.getName()));
const indexSpec = {
    a: "hashed"
};

// Test unique index not created (maybe change later).
assert.commandFailedWithCode(t.createIndex(indexSpec, {"unique": true}), 16764);
assert.eq(t.getIndexes().length, 1, "unique index got created.");

// Test compound hashed indexes work.
assert.commandWorked(t.createIndex(indexSpec));
assert.eq(t.getIndexes().length, 2);

// Test basic inserts.
for (let i = 0; i < 10; i++) {
    assert.commandWorked(t.insert({a: i}));
}
assert.eq(t.find().count(), 10, "basic insert didn't work");
assert.eq(t.find().hint(indexSpec).toArray().length, 10, "basic insert didn't work");
assert.eq(t.find({a: 3}).hint({_id: 1}).toArray()[0]._id,
          t.find({a: 3}).hint(indexSpec).toArray()[0]._id,
          "hashindex lookup didn't work");

// Make sure things with the same hash are not both returned.
assert.commandWorked(t.insert({a: 3.1}));
assert.eq(t.find().count(), 11, "additional insert didn't work");
assert.eq(t.find({a: 3.1}).hint(indexSpec).toArray().length, 1);
assert.eq(t.find({a: 3}).hint(indexSpec).toArray().length, 1);
// Test right obj is found.
assert.eq(t.find({a: 3.1}).hint(indexSpec).toArray()[0].a, 3.1);

// Make sure we're using the hashed index.
let explain = t.find({a: 1}).explain();
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)), "not using hashed index");

explain = t.find({c: 1}).explain();
assert(!isIxscan(db, getWinningPlan(explain.queryPlanner)), "using irrelevant hashed index");

// Hash index used with a $in set membership predicate.
explain = t.find({a: {$in: [1, 2]}}).explain();
printjson(explain);
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)), "not using hashed index");

// Hash index used with a singleton $and predicate conjunction.
explain = t.find({$and: [{a: 1}]}).explain();
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)), "not using hashed index");

// Hash index used with a non singleton $and predicate conjunction.
explain = t.find({$and: [{a: {$in: [1, 2]}}, {a: {$gt: 1}}]}).explain();
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)), "not using hashed index");

// Non-sparse hashed index can be used to satisfy {$exists: false}.
explain = t.find({a: {$exists: false}}).explain();
assert(isIxscan(db, getWinningPlan(explain.queryPlanner)), explain);

// Test creation of index based on hash of _id index.
const indexSpec2 = {
    '_id': "hashed"
};
assert.commandWorked(t.createIndex(indexSpec2));
assert.eq(t.getIndexes().length, 3, "_id index didn't get created");

const newid = t.findOne()["_id"];
assert.eq(t.find({_id: newid}).hint({_id: 1}).toArray()[0]._id,
          t.find({_id: newid}).hint(indexSpec2).toArray()[0]._id,
          "using hashed index and different index returns different docs");

// Test creation of sparse hashed index.
const sparseIndex = {
    b: "hashed"
};
assert.commandWorked(t.createIndex(sparseIndex, {"sparse": true}));
assert.eq(t.getIndexes().length, 4, "sparse index didn't get created");

// Sparse hashed indexes cannot be used to satisfy {$exists: false}.
explain = t.find({b: {$exists: false}}).explain();
assert(!isIxscan(db, getWinningPlan(explain.queryPlanner)), explain);

// Test sparse index has smaller total items on after inserts.
if (!TestData.isHintsToQuerySettingsSuite) {
    for (let i = 0; i < 10; i++) {
        assert.commandWorked(t.insert({b: i}));
    }
    const totalb = t.find().hint(sparseIndex).toArray().length;
    assert.eq(totalb, 10, "sparse index has wrong total");

    const total = t.find().hint({"_id": 1}).toArray().length;
    const totala = t.find().hint(indexSpec).toArray().length;
    assert.eq(total, totala, "non-sparse index has wrong total");
    assert.lt(totalb, totala, "sparse index should have smaller total");
}

// Test that having arrays along the path of the index is not allowed.
assert.commandWorked(t.createIndex({"field1.field2.0.field4": "hashed"}));
assert.commandFailedWithCode(t.insert({field1: []}), 16766);
assert.commandFailedWithCode(t.insert({field1: {field2: []}}), 16766);
assert.commandFailedWithCode(t.insert({field1: {field2: {0: []}}}), 16766);
assert.commandFailedWithCode(t.insert({field1: [{field2: {0: []}}]}), 16766);
assert.commandFailedWithCode(t.insert({field1: {field2: {0: {field4: []}}}}), 16766);
assert.commandWorked(t.insert({field1: {field2: {0: {otherField: []}}}}));
assert.commandWorked(t.insert({field1: {field2: {0: {field4: 1}}}}));
