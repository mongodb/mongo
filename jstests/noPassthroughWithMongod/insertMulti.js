// check the insertMulti path works, including the error handling

(function() {
"use strict";

function makeDocument(docSize) {
    var doc = {"fieldName": ""};
    var longString = "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    while (Object.bsonsize(doc) < docSize) {
        if (Object.bsonsize(doc) < docSize - longString.length) {
            doc.fieldName += longString;
        } else {
            doc.fieldName += "x";
        }
    }
    return doc;
}

var t = db.foo;

t.drop();
assert.commandWorked(t.insert([{_id: 1}, {_id: 2}]));
assert.eq(t.count(), 2);
// no ContinueOnError
assert.commandFailedWithCode(t.insert([{_id: 3}, {_id: 2}, {_id: 4}], 0), ErrorCodes.DuplicateKey);
assert.eq(t.count(), 3);
assert.eq(t.count({"_id": 1}), 1);
assert.eq(t.count({"_id": 2}), 1);
assert.eq(t.count({"_id": 3}), 1);
assert.eq(t.count({"_id": 4}), 0);

assert(t.drop());
assert.commandWorked(t.insert([{_id: 1}, {_id: 2}]));
assert.eq(t.count(), 2);
// ContinueOnError
assert.commandFailedWithCode(t.insert([{_id: 3}, {_id: 2}, {_id: 4}], 1), ErrorCodes.DuplicateKey);
assert.eq(t.count(), 4);
assert.eq(t.count({"_id": 1}), 1);
assert.eq(t.count({"_id": 2}), 1);
assert.eq(t.count({"_id": 3}), 1);
assert.eq(t.count({"_id": 4}), 1);

// Push a large vector in bigger than the subset size we'll break it up into
assert(t.drop());
var doc = makeDocument(16 * 1024);
var docs = [];
for (var i = 0; i < 1000; i++)
    docs.push(Object.extend({}, doc));
assert.commandWorked(t.insert(docs));
assert.eq(t.count(), docs.length);

assert(t.drop());
})();
