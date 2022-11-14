/**
 * Tests the bsonUnorderedFieldsCompare function.
 */
(function() {
"use strict";

const tests = [];

tests.push(function compareOrderedFieldsSameDoc() {
    const doc = {_id: 1, field1: 1, field2: "a"};
    assert.eq(0, bsonUnorderedFieldsCompare(doc, doc), "identical docs were not equal");
});

tests.push(function compareUnorderedFieldsSameDoc() {
    const doc1 = {_id: 1, field1: 1, field2: "a"};
    const doc2 = {_id: 1, field2: "a", field1: 1};
    assert.eq(0,
              bsonUnorderedFieldsCompare(doc1, doc2),
              "docs with same fields but out of order were not equal");
});

tests.push(function compareOrderedFieldsDifferentDoc() {
    const doc1 = {_id: 1, field1: 1, field2: "a"};
    const doc2 = {_id: 1, field1: 1, field2: "b"};
    assert.neq(0, bsonUnorderedFieldsCompare(doc1, doc2), "docs with different fields were equal");
});

tests.push(function compareUnorderedFieldsDifferentDoc() {
    const doc1 = {_id: 1, field1: 1, field2: "a"};
    const doc2 = {_id: 1, field2: "b", field1: 1};
    assert.neq(0,
               bsonUnorderedFieldsCompare(doc1, doc2),
               "docs with different fields with different field orders were equal");
});

// Run each test.
tests.forEach(test => {
    test();
});
})();
