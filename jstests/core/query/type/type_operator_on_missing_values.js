// Test $type expression with non-existent field in the document.

(function() {
"use strict";

const coll = db.type_operator_on_missing_values;
coll.drop();

const documentList = [
    {_id: 0},
    {_id: 1, b: 123},
];
assert.commandWorked(coll.insert(documentList));

const bsonTypes = [
    "double",    "string",    "object",     "array",   "binData",
    "undefined", "objectId",  "bool",       "date",    "null",
    "regex",     "dbPointer", "javascript", "symbol",  "javascriptWithScope",
    "int",       "timestamp", "long",       "decimal", "minKey",
    "maxKey",
];

for (const type of bsonTypes) {
    let results = coll.find({a: {$type: type}}).sort({_id: 1}).toArray();
    assert.eq(results, []);

    results = coll.find({a: {$not: {$type: type}}}).sort({_id: 1}).toArray();
    assert.eq(results, documentList);
}
}());
