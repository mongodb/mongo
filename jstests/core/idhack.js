
t = db.idhack;
t.drop();

// Include helpers for analyzing explain output.
load("jstests/libs/analyze_plan.js");

t.insert({_id: {x: 1}, z: 1});
t.insert({_id: {x: 2}, z: 2});
t.insert({_id: {x: 3}, z: 3});
t.insert({_id: 1, z: 4});
t.insert({_id: 2, z: 5});
t.insert({_id: 3, z: 6});

assert.eq(2, t.findOne({_id: {x: 2}}).z, "A1");
assert.eq(2, t.find({_id: {$gte: 2}}).count(), "A2");
assert.eq(2, t.find({_id: {$gte: 2}}).itcount(), "A3");

t.update({_id: {x: 2}}, {$set: {z: 7}});
assert.eq(7, t.findOne({_id: {x: 2}}).z, "B1");

t.update({_id: {$gte: 2}}, {$set: {z: 8}}, false, true);
assert.eq(4, t.findOne({_id: 1}).z, "C1");
assert.eq(8, t.findOne({_id: 2}).z, "C2");
assert.eq(8, t.findOne({_id: 3}).z, "C3");

// explain output should show that the ID hack was applied.
var query = {_id: {x: 2}};
var explain = t.find(query).explain(true);
print("explain for " + tojson(query, "", true) + " = " + tojson(explain));
assert.eq(1, explain.executionStats.nReturned, "D1");
assert.eq(1, explain.executionStats.totalKeysExamined, "D2");
assert(isIdhack(explain.queryPlanner.winningPlan), "D3");

// ID hack cannot be used with hint().
t.ensureIndex({_id: 1, a: 1});
var hintExplain = t.find(query).hint({_id: 1, a: 1}).explain();
print("explain for hinted query = " + tojson(hintExplain));
assert(!isIdhack(hintExplain.queryPlanner.winningPlan), "E1");

// ID hack cannot be used with skip().
var skipExplain = t.find(query).skip(1).explain();
print("explain for skip query = " + tojson(skipExplain));
assert(!isIdhack(skipExplain.queryPlanner.winningPlan), "F1");

// Covered query returning _id field only can be handled by ID hack.
var coveredExplain = t.find(query, {_id: 1}).explain();
print("explain for covered query = " + tojson(coveredExplain));
assert(isIdhack(coveredExplain.queryPlanner.winningPlan), "G1");
// Check doc from covered ID hack query.
assert.eq({_id: {x: 2}}, t.findOne(query, {_id: 1}), "G2");

//
// Non-covered projection for idhack.
//

t.drop();
t.insert({_id: 0, a: 0, b: [{c: 1}, {c: 2}]});
t.insert({_id: 1, a: 1, b: [{c: 3}, {c: 4}]});

// Simple inclusion.
assert.eq({_id: 1, a: 1}, t.find({_id: 1}, {a: 1}).next());
assert.eq({a: 1}, t.find({_id: 1}, {_id: 0, a: 1}).next());
assert.eq({_id: 0, a: 0}, t.find({_id: 0}, {_id: 1, a: 1}).next());

// Non-simple: exclusion.
assert.eq({_id: 1, a: 1}, t.find({_id: 1}, {b: 0}).next());
assert.eq({
    _id: 0,
},
          t.find({_id: 0}, {a: 0, b: 0}).next());

// Non-simple: dotted fields.
assert.eq({b: [{c: 1}, {c: 2}]}, t.find({_id: 0}, {_id: 0, "b.c": 1}).next());
assert.eq({_id: 1}, t.find({_id: 1}, {"foo.bar": 1}).next());

// Non-simple: elemMatch projection.
assert.eq({_id: 1, b: [{c: 4}]}, t.find({_id: 1}, {b: {$elemMatch: {c: 4}}}).next());

// Non-simple: .returnKey().
assert.eq({_id: 1}, t.find({_id: 1}).returnKey().next());

// Non-simple: .returnKey() overrides other projections.
assert.eq({_id: 1}, t.find({_id: 1}, {a: 1}).returnKey().next());
