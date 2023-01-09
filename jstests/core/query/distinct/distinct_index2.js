(function() {
'use strict';

// Uniformly distributed dataset.
// If we use a randomly generated dataset, we might not
// generate all the distinct values in the range [0, k).
const k = 10;
let docs = [];
let docId = 0;
for (let a = 0; a < k; a++) {
    for (let b = 0; b < k; b++) {
        for (let c = 0; c < k; c++) {
            docs.push({_id: docId++, a: a, b: b, c: c});
        }
    }
}

const collNamePrefix = 'distinct_index2_';
let collCount = 0;

const correct = Array.from({length: k}, (_, i) => i);
function check(field) {
    let res = t.distinct(field);
    assert.sameMembers(correct, res, "check: " + field + "; coll: " + t.getFullName());

    if (field != "a") {
        res = t.distinct(field, {a: 1});
        assert.sameMembers(correct, res, "check 2: " + field + "; coll: " + t.getFullName());
    }
}

let t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndexes([{a: 1, b: 1}, {c: 1}]));
assert.commandWorked(t.insert(docs));

check("a");
check("b");
check("c");

// hashed index should produce same results.
t = db.getCollection(collNamePrefix + collCount++);
t.drop();
assert.commandWorked(t.createIndex({a: "hashed"}));
assert.commandWorked(t.insert(docs));
check("a");
})();
