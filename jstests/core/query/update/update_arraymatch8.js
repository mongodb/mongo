// Checking for positional array updates with either .$ or .0 at the end
// SERVER-7511

// array.$.name
let t = db[jsTestName()];
t.drop();
assert.commandWorked(t.createIndex({'array.name': 1}));
assert.commandWorked(t.insert({'array': [{'name': 'old'}]}));
assert(t.findOne({'array.name': 'old'}));
assert.commandWorked(t.update({'array.name': 'old'}, {$set: {'array.$.name': 'new'}}));
assert(t.findOne({'array.name': 'new'}));
assert(!t.findOne({'array.name': 'old'}));

// array.$   (failed in 2.2.2)
t = db[jsTestName()];
assert(t.drop());
assert.commandWorked(t.createIndex({'array.name': 1}));
assert.commandWorked(t.insert({'array': [{'name': 'old'}]}));
assert(t.findOne({'array.name': 'old'}));
assert.commandWorked(t.update({'array.name': 'old'}, {$set: {'array.$': {'name': 'new'}}}));
assert(t.findOne({'array.name': 'new'}));
assert(!t.findOne({'array.name': 'old'}));

// array.0.name
t = db[jsTestName()];
assert(t.drop());
assert.commandWorked(t.createIndex({'array.name': 1}));
assert.commandWorked(t.insert({'array': [{'name': 'old'}]}));
assert(t.findOne({'array.name': 'old'}));
assert.commandWorked(t.update({'array.name': 'old'}, {$set: {'array.0.name': 'new'}}));
assert(t.findOne({'array.name': 'new'}));
assert(!t.findOne({'array.name': 'old'}));

// array.0   (failed in 2.2.2)
t = db[jsTestName()];
assert(t.drop());
assert.commandWorked(t.createIndex({'array.name': 1}));
assert.commandWorked(t.insert({'array': [{'name': 'old'}]}));
assert(t.findOne({'array.name': 'old'}));
assert.commandWorked(t.update({'array.name': 'old'}, {$set: {'array.0': {'name': 'new'}}}));
assert(t.findOne({'array.name': 'new'}));
assert(!t.findOne({'array.name': 'old'}));

// // array.12.name
t = db[jsTestName()];
assert(t.drop());
let arr = new Array();
for (var i = 0; i < 20; i++) {
    arr.push({'name': 'old'});
}
assert.commandWorked(t.createIndex({'array.name': 1}));
assert.commandWorked(t.insert({_id: 0, 'array': arr}));
assert(t.findOne({'array.name': 'old'}));
assert.commandWorked(t.update({_id: 0}, {$set: {'array.12.name': 'new'}}));
// note: both documents now have to be in the array
assert(t.findOne({'array.name': 'new'}));
assert(t.findOne({'array.name': 'old'}));

// array.12   (failed in 2.2.2)
t = db[jsTestName()];
assert(t.drop());
arr = new Array();
for (var i = 0; i < 20; i++) {
    arr.push({'name': 'old'});
}
assert.commandWorked(t.createIndex({'array.name': 1}));
assert.commandWorked(t.insert({_id: 0, 'array': arr}));
assert(t.findOne({'array.name': 'old'}));
assert.commandWorked(t.update({_id: 0}, {$set: {'array.12': {'name': 'new'}}}));
// note: both documents now have to be in the array
assert(t.findOne({'array.name': 'new'}));
assert(t.findOne({'array.name': 'old'}));

// array.$.123a.name
t = db.jstests_update_arraymatch8;
assert(t.drop());
assert.commandWorked(t.createIndex({'array.123a.name': 1}));
assert.commandWorked(t.insert({'array': [{'123a': {'name': 'old'}}]}));
assert(t.findOne({'array.123a.name': 'old'}));
assert.commandWorked(t.update({'array.123a.name': 'old'}, {$set: {'array.$.123a.name': 'new'}}));
assert(t.findOne({'array.123a.name': 'new'}));
assert(!t.findOne({'array.123a.name': 'old'}));

// array.$.123a
t = db.jstests_update_arraymatch8;
assert(t.drop());
assert.commandWorked(t.createIndex({'array.name': 1}));
assert.commandWorked(t.insert({'array': [{'123a': {'name': 'old'}}]}));
assert(t.findOne({'array.123a.name': 'old'}));
assert.commandWorked(
    t.update({'array.123a.name': 'old'}, {$set: {'array.$.123a': {'name': 'new'}}}));
assert(t.findOne({'array.123a.name': 'new'}));
assert(!t.findOne({'array.123a.name': 'old'}));

// array.0.123a.name
t = db.jstests_update_arraymatch8;
assert(t.drop());
assert.commandWorked(t.createIndex({'array.123a.name': 1}));
assert.commandWorked(t.insert({'array': [{'123a': {'name': 'old'}}]}));
assert(t.findOne({'array.123a.name': 'old'}));
assert.commandWorked(t.update({'array.123a.name': 'old'}, {$set: {'array.0.123a.name': 'new'}}));
assert(t.findOne({'array.123a.name': 'new'}));
assert(!t.findOne({'array.123a.name': 'old'}));

// array.0.123a
t = db.jstests_update_arraymatch8;
assert(t.drop());
assert.commandWorked(t.createIndex({'array.name': 1}));
assert.commandWorked(t.insert({'array': [{'123a': {'name': 'old'}}]}));
assert(t.findOne({'array.123a.name': 'old'}));
assert.commandWorked(
    t.update({'array.123a.name': 'old'}, {$set: {'array.0.123a': {'name': 'new'}}}));
assert(t.findOne({'array.123a.name': 'new'}));
assert(!t.findOne({'array.123a.name': 'old'}));

// a.0.b
t = db.jstests_update_arraymatch8;
assert(t.drop());
assert.commandWorked(t.createIndex({'a.0.b': 1}));
assert.commandWorked(t.insert({'a': [[{b: 'old'}]]}));
assert(t.findOne({'a.0.0.b': 'old'}));
assert(t.findOne({'a.0.b': 'old'}));
assert.commandWorked(t.update({}, {$set: {'a.0.0.b': 'new'}}));
assert(t.findOne({'a.0.b': 'new'}));
assert(!t.findOne({'a.0.b': 'old'}));

// a.0.b.c
t = db.jstests_update_arraymatch8;
assert(t.drop());
assert.commandWorked(t.createIndex({'a.0.b.c': 1}));
assert.commandWorked(t.insert({'a': [{b: [{c: 'old'}]}]}));
assert(t.findOne({'a.0.b.0.c': 'old'}));
assert(t.findOne({'a.b.0.c': 'old'}));
assert(t.findOne({'a.0.b.c': 'old'}));
assert(t.findOne({'a.b.c': 'old'}));
assert.commandWorked(t.update({}, {$set: {'a.0.b.0.c': 'new'}}));
assert(t.findOne({'a.0.b.c': 'new'}));
assert(!t.findOne({'a.0.b.c': 'old'}));

// a.b.$ref
t = db.jstests_update_arraymatch8;
assert(t.drop());
assert.commandWorked(t.createIndex({'a.b.$ref': 1}));
assert.commandWorked(t.insert({'a': [{'b': {'$ref': 'old', '$id': 0}}]}));
assert(t.findOne({'a.b.$ref': 'old'}));
assert(t.findOne({'a.0.b.$ref': 'old'}));
assert.commandWorked(t.update({}, {$set: {'a.0.b.$ref': 'new'}}));
assert(t.findOne({'a.b.$ref': 'new'}));
assert(!t.findOne({'a.b.$ref': 'old'}));

// a.b and a-b
t = db[jsTestName()];
assert(t.drop());
assert.commandWorked(t.createIndex({'a.b': 1}));
assert.commandWorked(t.createIndex({'a-b': 1}));
assert.commandWorked(t.insert({'a': {'b': 'old'}}));
assert(t.findOne({'a.b': 'old'}));
assert.commandWorked(t.update({}, {$set: {'a': {'b': 'new'}}}));
assert(t.findOne({'a.b': 'new'}));
assert(!t.findOne({'a.b': 'old'}));
