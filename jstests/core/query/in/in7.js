// Uassert when $elemMatch is attempted within $in SERVER-3545

t = db.jstests_ina;
t.drop();
t.save({});

assert.throws(function() {
    t.find({a: {$in: [{$elemMatch: {b: 1}}]}}).itcount();
});
assert.throws(function() {
    t.find({a: {$not: {$in: [{$elemMatch: {b: 1}}]}}}).itcount();
});

assert.throws(function() {
    t.find({a: {$nin: [{$elemMatch: {b: 1}}]}}).itcount();
});
assert.throws(function() {
    t.find({a: {$not: {$nin: [{$elemMatch: {b: 1}}]}}}).itcount();
});

// NOTE Above we don't check cases like {b:2,$elemMatch:{b:3,4}} - generally
// we assume that the first key is $elemMatch if any key is, and validating
// every key is expensive in some cases.