// don't allow most operators with regex

t = db.jstests_ne3;
t.drop();

assert.throws(function() {
    t.findOne({t: {$ne: /a/}});
});
assert.throws(function() {
    t.findOne({t: {$gt: /a/}});
});
assert.throws(function() {
    t.findOne({t: {$gte: /a/}});
});
assert.throws(function() {
    t.findOne({t: {$lt: /a/}});
});
assert.throws(function() {
    t.findOne({t: {$lte: /a/}});
});

assert.eq(0, t.count({t: {$in: [/a/]}}));
