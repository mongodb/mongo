// Cannot implicitly shard accessed collections because of following errmsg: A single
// update/delete on a sharded collection must contain an exact match on _id or contain the shard
// key.
//
// @tags: [assumes_unsharded_collection, requires_fcv_50]

(function() {
"use strict";

const coll = db.update_addToSet;
coll.drop();

let doc = {_id: 1, a: [2, 1]};
assert.commandWorked(coll.insert(doc));

assert.eq(doc, coll.findOne());

assert.commandWorked(coll.update({}, {$addToSet: {a: 3}}));
doc.a.push(3);
assert.eq(doc, coll.findOne());

coll.update({}, {$addToSet: {a: 3}});
assert.eq(doc, coll.findOne());

// SERVER-628
assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: [3, 5, 6]}}}));
doc.a.push(5);
doc.a.push(6);
assert.eq(doc, coll.findOne());

assert(coll.drop());
doc = {
    _id: 1,
    a: [3, 5, 6]
};
assert.commandWorked(coll.insert(doc));
assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: [3, 5, 6]}}}));
assert.eq(doc, coll.findOne());

assert(coll.drop());
assert.commandWorked(coll.update({_id: 1}, {$addToSet: {a: {$each: [3, 5, 6]}}}, true));
assert.eq(doc, coll.findOne());
assert.commandWorked(coll.update({_id: 1}, {$addToSet: {a: {$each: [3, 5, 6]}}}, true));
assert.eq(doc, coll.findOne());

// SERVER-630
assert(coll.drop());
assert.commandWorked(coll.update({_id: 2}, {$addToSet: {a: 3}}, true));
assert.eq(1, coll.find({}).itcount());
assert.eq({_id: 2, a: [3]}, coll.findOne());

// SERVER-3245
doc = {
    _id: 1,
    a: [1, 2]
};
assert(coll.drop());
assert.commandWorked(coll.update({_id: 1}, {$addToSet: {a: {$each: [1, 2]}}}, true));
assert.eq(doc, coll.findOne());

assert(coll.drop());
assert.commandWorked(coll.update({_id: 1}, {$addToSet: {a: {$each: [1, 2, 1, 2]}}}, true));
assert.eq(doc, coll.findOne());

assert(coll.drop());
assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(coll.update({_id: 1}, {$addToSet: {a: {$each: [1, 2, 2, 1]}}}));
assert.eq(doc, coll.findOne());

assert.commandWorked(coll.update({_id: 1}, {$addToSet: {a: {$each: [3, 2, 2, 3, 3]}}}));
doc.a.push(3);
assert.eq(doc, coll.findOne());

var isDotsAndDollarsEnabled = db.adminCommand({getParameter: 1, featureFlagDotsAndDollars: 1})
                                  .featureFlagDotsAndDollars.value;
if (!isDotsAndDollarsEnabled) {
    // Test that dotted and '$' prefixed field names fail.
    assert(coll.drop());
    doc = {_id: 1, a: [1, 2]};
    assert.commandWorked(coll.insert(doc));

    assert.commandWorked(coll.update({}, {$addToSet: {a: {'x.$.y': 'bad'}}}));
    assert.commandWorked(coll.update({}, {$addToSet: {a: {b: {'x.$.y': 'bad'}}}}));

    assert.commandFailed(coll.update({}, {$addToSet: {a: {"$bad": "bad"}}}));
    assert.commandFailed(coll.update({}, {$addToSet: {a: {b: {"$bad": "bad"}}}}));

    assert.commandWorked(coll.update({}, {$addToSet: {a: {_id: {"x.y": 2}}}}));

    assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: [{'x.$.y': 'bad'}]}}}));
    assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: [{b: {'x.$.y': 'bad'}}]}}}));

    assert.commandFailed(coll.update({}, {$addToSet: {a: {$each: [{'$bad': 'bad'}]}}}));
    assert.commandFailed(coll.update({}, {$addToSet: {a: {$each: [{b: {'$bad': 'bad'}}]}}}));
} else {
    // Test that dotted and '$' prefixed field names work when nested.
    assert(coll.drop());
    doc = {_id: 1, a: [1, 2]};
    assert.commandWorked(coll.insert(doc));

    assert.commandWorked(coll.update({}, {$addToSet: {a: {'x.$.y': 'bad'}}}));
    assert.commandWorked(coll.update({}, {$addToSet: {a: {b: {'x.$.y': 'bad'}}}}));

    assert.commandWorked(coll.update({}, {$addToSet: {a: {"$bad": "bad"}}}));
    assert.commandWorked(coll.update({}, {$addToSet: {a: {b: {"$bad": "bad"}}}}));

    assert.commandWorked(coll.update({}, {$addToSet: {a: {_id: {"x.y": 2}}}}));

    assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: [{'x.$.y': 'bad'}]}}}));
    assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: [{b: {'x.$.y': 'bad'}}]}}}));

    assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: [{'$bad': 'bad'}]}}}));
    assert.commandWorked(coll.update({}, {$addToSet: {a: {$each: [{b: {'$bad': 'bad'}}]}}}));
}

// Test that nested _id fields are allowed.
assert(coll.drop());
doc = {
    _id: 1,
    a: [1, 2]
};
assert.commandWorked(coll.insert(doc));

assert.commandWorked(coll.update({}, {$addToSet: {a: {_id: ["foo", "bar", "baz"]}}}));
assert.commandWorked(coll.update({}, {$addToSet: {a: {_id: /acme.*corp/}}}));

// Test that DBRefs are allowed.
assert(coll.drop());
doc = {
    _id: 1,
    a: [1, 2]
};
assert.commandWorked(coll.insert(doc));

const foo = {
    "foo": "bar"
};
assert.commandWorked(coll.insert(foo));
const fooDoc = coll.findOne(foo);
assert.eq(fooDoc.foo, foo.foo);

const fooDocRef = {
    reference: new DBRef(coll.getName(), fooDoc._id, coll.getDB().getName())
};

assert.commandWorked(coll.update({_id: doc._id}, {$addToSet: {a: fooDocRef}}));
assert.eq(coll.findOne({_id: doc._id}).a[2], fooDocRef);

assert.commandWorked(coll.update({_id: doc._id}, {$addToSet: {a: {b: fooDocRef}}}));
assert.eq(coll.findOne({_id: doc._id}).a[3].b, fooDocRef);
}());
