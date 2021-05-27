// @tags: [
//   requires_non_retryable_writes,
// ]

// Test index key generation with duplicate values addressed by array index and
// object field.  SERVER-2902

(function() {
'use strict';

const collNamePrefix = 'jstests_indexu_';
let collCount = 0;
let t = db.getCollection(collNamePrefix + collCount++);
t.drop();

const dupDoc = {
    _id: 0,
    a: [{'0': 1}]
};  // There are two 'a.0' fields in this doc.
const dupDoc2 = {
    a: [{'1': 1}, 'c']
};
const noDupDoc = {
    a: [{'1': 1}]
};

// Test that we can't index dupDoc.
assert.commandWorked(t.save(dupDoc));
assert.commandFailedWithCode(t.createIndex({'a.0': 1}), 16746);

// Test that we can fail gracefully when dupDoc has a large array padded with nulls.
// Index is based on max padding constant in mongo/db/update/path_support.h
assert.commandWorked(t.update({_id: 0}, {$set: {'a.1500001': 1}}));
assert.commandFailedWithCode(t.createIndex({'a.0': 1}), 16746);

t.remove({});
assert.commandWorked(t.createIndex({'a.0': 1}));
assert.writeError(t.save(dupDoc));

// Test that we can't index dupDoc2.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.save(dupDoc2));
assert.commandFailedWithCode(t.createIndex({'a.1': 1}), 16746);

t.remove({});
assert.commandWorked(t.createIndex({'a.1': 1}));
assert.writeError(t.save(dupDoc2));

// Test that we can index dupDoc with a different index.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.createIndex({'a.b': 1});
assert.commandWorked(t.save(dupDoc));

// Test number field starting with hyphen.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.createIndex({'a.-1': 1});
assert.commandWorked(t.save({a: [{'-1': 1}]}));

// Test number field starting with zero.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.createIndex({'a.00': 1});
assert.commandWorked(t.save({a: [{'00': 1}]}));

// Test multiple array indexes
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.createIndex({'a.0': 1, 'a.1': 1});
assert.commandWorked(t.save({a: [{'1': 1}]}));
assert.writeError(t.save({a: [{'1': 1}, 4]}));

// Test that we can index noDupDoc.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.save(noDupDoc);
assert.commandWorked(t.createIndex({'a.0': 1}));
assert.commandWorked(t.createIndex({'a.1': 1}));

t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.createIndex({'a.0': 1});
t.createIndex({'a.1': 1});
assert.commandWorked(t.save(noDupDoc));

// Test that we can query noDupDoc.
assert.eq(1, t.find({'a.1': 1}).hint({'a.1': 1}).itcount());
assert.eq(1, t.find({'a.1': 1}).hint({$natural: 1}).itcount());
assert.eq(1, t.find({'a.0': {'1': 1}}).hint({'a.0': 1}).itcount());
assert.eq(1, t.find({'a.0': {'1': 1}}).hint({$natural: 1}).itcount());

// Check multiple nested array fields.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.save({a: [[1]]});
assert.commandWorked(t.createIndex({'a.0.0': 1}));
assert.eq(1, t.find({'a.0.0': 1}).hint({$natural: 1}).itcount());
assert.eq(1, t.find({'a.0.0': 1}).hint({'a.0.0': 1}).itcount());

// Check where there is a duplicate for a partially addressed field but not for a fully addressed
// field.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.save({a: [[1], {'0': 1}]});
assert.commandFailed(t.createIndex({'a.0.0': 1}));

// Check where there is a duplicate for a fully addressed field.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.save({a: [[1], {'0': [1]}]}));
assert.commandFailedWithCode(t.createIndex({'a.0.0': 1}), 16746);

// Two ways of addressing parse to an array.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.save({a: [{'0': 1}]});
assert.commandFailedWithCode(t.createIndex({'a.0.0': 1}), 16746);

// Test several key depths - with same arrays being found.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.save({a: [{'0': [{'0': 1}]}]});
assert.commandFailedWithCode(t.createIndex({'a.0.0.0.0.0.0': 1}), 16746);
assert.commandFailedWithCode(t.createIndex({'a.0.0.0.0.0': 1}), 16746);
assert.commandFailedWithCode(t.createIndex({'a.0.0.0.0': 1}), 16746);
assert.commandFailedWithCode(t.createIndex({'a.0.0.0': 1}), 16746);
assert.commandFailedWithCode(t.createIndex({'a.0.0': 1}), 16746);
assert.commandFailedWithCode(t.createIndex({'a.0': 1}), 16746);
assert.commandWorked(t.createIndex({'a': 1}));

// Two prefixes extract docs, but one terminates extraction before array.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.save({a: [{'0': {'c': []}}]});
assert.commandFailedWithCode(t.createIndex({'a.0.c': 1}), 16746);

t = db.getCollection(collNamePrefix + collCount++);
t.drop();
t.save({a: [[{'b': 1}]]});
assert.eq(1, t.find({'a.0.b': 1}).itcount());
t.createIndex({'a.0.b': 1});
assert.eq(1, t.find({'a.0.b': 1}).itcount());
})();
