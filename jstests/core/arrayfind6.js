// Check index bound determination for $not:$elemMatch queries.  SERVER-5740

(function() {
'use strict';

let t = db.jstests_arrayfind6;
t.drop();

assert.commandWorked(t.save({a: [{b: 1, c: 2}]}));

function checkElemMatchMatches() {
    assert.eq(1, t.count({a: {$elemMatch: {b: 1, c: 2}}}));
    assert.eq(0, t.count({a: {$not: {$elemMatch: {b: 1, c: 2}}}}));
    assert.eq(1, t.count({a: {$not: {$elemMatch: {b: 1, c: 3}}}}));
    assert.eq(1, t.count({a: {$not: {$elemMatch: {b: {$ne: 1}, c: 3}}}}));
    // Index bounds must be determined for $not:$elemMatch, not $not:$ne.  In this case if index
    // bounds are determined for $not:$ne, the a.b index will be constrained to the interval [2,2]
    // and the saved document will not be matched as it should.
    assert.eq(1, t.count({a: {$not: {$elemMatch: {b: {$ne: 2}, c: 3}}}}));
}

checkElemMatchMatches();
assert.commandWorked(t.createIndex({'a.b': 1}));
checkElemMatchMatches();

// SBE bug SERVER-54332
t.remove({});
assert.commandWorked(t.insertMany([{a: []}, {a: [1, 2, 3]}, {a: [{c: 1}]}]));
assert.eq(3, t.find({a: {$not: {$elemMatch: {b: 1}}}}).itcount());
})();
