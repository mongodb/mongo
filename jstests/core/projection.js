/**
 * Tests for FieldMatcher (later renamed to Projection).
 */

(function() {
'use strict';
const collNamePrefix = 'jstests_projection_';
let collCount = 0;

let res;

//
// Test cases originally in fm1.js.
//

let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

let doc = {_id: 0, foo: {bar: 1}};
assert.commandWorked(t.insert(doc));

res = t.find({}, {foo: 1}).toArray();
assert.eq(1, res.length, tojson(res));
assert.docEq(doc, res[0], tojson(res));

res = t.find({}, {'foo.bar': 1}).toArray();
assert.eq(1, res.length, tojson(res));
assert.docEq(doc, res[0], tojson(res));

res = t.find({}, {'baz': 1}).toArray();
assert.eq(1, res.length, tojson(res));
assert.docEq({_id: 0}, res[0], tojson(res));

res = t.find({}, {'baz.qux': 1}).toArray();
assert.eq(1, res.length, tojson(res));
assert.docEq({_id: 0}, res[0], tojson(res));

res = t.find({}, {'foo.qux': 1}).toArray();
assert.eq(1, res.length, tojson(res));
assert.docEq({_id: 0, foo: {}}, res[0], tojson(res));

//
// Test cases originally in fm2.js
//

t = db.getCollection(collNamePrefix + collCount++);
t.drop();

doc = {
    _id: 1,
    one: {two: {three: 'four'}}
};
assert.commandWorked(t.insert(doc));

res = t.find({}, {'one.two': 1}).toArray();
assert.eq(1, res.length, tojson(res));
assert.docEq(doc, res[0], tojson(res));
assert.eq(1, Object.keySet(res[0].one).length, tojson(res));

//
// Test cases originally in fm3.js.
//

t = db.getCollection(collNamePrefix + collCount++);
t.drop();

assert.commandWorked(t.insert({a: [{c: {e: 1, f: 1}}, {d: 2}, 'z'], b: 1}));

res = t.findOne({}, {a: 1});
assert.eq(res.a, [{c: {e: 1, f: 1}}, {d: 2}, 'z'], "one a");
assert.eq(res.b, undefined, "one b");

res = t.findOne({}, {a: 0});
assert.eq(res.a, undefined, "two a");
assert.eq(res.b, 1, "two b");

res = t.findOne({}, {'a.d': 1});
assert.eq(res.a, [{}, {d: 2}], "three a");
assert.eq(res.b, undefined, "three b");

res = t.findOne({}, {'a.d': 0});
assert.eq(res.a, [{c: {e: 1, f: 1}}, {}, 'z'], "four a");
assert.eq(res.b, 1, "four b");

res = t.findOne({}, {'a.c': 1});
assert.eq(res.a, [{c: {e: 1, f: 1}}, {}], "five a");
assert.eq(res.b, undefined, "five b");

res = t.findOne({}, {'a.c': 0});
assert.eq(res.a, [{}, {d: 2}, 'z'], "six a");
assert.eq(res.b, 1, "six b");

res = t.findOne({}, {'a.c.e': 1});
assert.eq(res.a, [{c: {e: 1}}, {}], "seven a");
assert.eq(res.b, undefined, "seven b");

res = t.findOne({}, {'a.c.e': 0});
assert.eq(res.a, [{c: {f: 1}}, {d: 2}, 'z'], "eight a");
assert.eq(res.b, 1, "eight b");

//
// Test cases originally in fm4.js
//

t = db.getCollection(collNamePrefix + collCount++);
t.drop();

assert.commandWorked(t.insert({_id: 3, a: 1, b: 1}));

assert.docEq(t.findOne({}, {_id: 1}), {_id: 3}, "1");
assert.docEq(t.findOne({}, {_id: 0}), {a: 1, b: 1}, "2");

assert.docEq(t.findOne({}, {_id: 1, a: 1}), {_id: 3, a: 1}, "3");
assert.docEq(t.findOne({}, {_id: 0, a: 1}), {a: 1}, "4");

assert.docEq(t.findOne({}, {_id: 0, a: 0}), {b: 1}, "6");
assert.docEq(t.findOne({}, {a: 0}), {_id: 3, b: 1}, "5");

assert.docEq(t.findOne({}, {_id: 1, a: 0}), {_id: 3, b: 1}, "7");
})();
