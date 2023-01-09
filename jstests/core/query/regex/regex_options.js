/**
 * Test regex options in a find context.
 * @tags: [requires_fcv_51]
 */
(function() {
'use strict';
const coll = db.jstests_regex_options;

coll.drop();
assert.commandWorked(coll.insert({a: 'foo'}));
assert.eq(1, coll.count({a: {$regex: /O/i}}));
assert.eq(1, coll.count({a: /O/i}));
assert.eq(1, coll.count({a: {$regex: 'O', $options: 'i'}}));

// Test with various invalid options and make sure they aren't ignored.
// After SERVER-26991 the u flag should no longer be considered invalid.
coll.drop();
assert.commandWorked(coll.insert({txt: 'hello_test'}));

// Test valid/invalid options.
assert.eq(1, coll.count({txt: {$regex: '^hello.*'}}));
assert.eq(1, coll.count({txt: {$regex: '^hello.*', $options: 'u'}}));
assert.commandFailedWithCode(
    assert.throws(() => coll.count({txt: {$regex: '^hello.*', $options: 'g'}})), 51108);

// Test using regex object.
assert.eq(1, coll.count({txt: {$regex: /^hello.*/}}));
assert.eq(1, coll.count({txt: {$regex: /^hello.*/u}}));
assert.commandFailedWithCode(assert.throws(() => coll.count({txt: {$regex: /^hello.*/g}})), 51108);

// Test in projection.
assert.eq(1, coll.find({}, {p: {$regexFind: {input: '$txt', regex: /^hello.*/}}}).toArray().length);
assert.eq(1,
          coll.find({}, {p: {$regexFind: {input: '$txt', regex: /^hello.*/u}}}).toArray().length);
assert.commandFailedWithCode(
    assert.throws(() => coll.find({}, {p: {$regexFind: {input: '$txt', regex: /^hello.*/g}}})
                            .itcount()),
                 51108);
})();
