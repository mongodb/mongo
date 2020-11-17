(function() {
"use strict";

const coll = db.array1;
coll.drop();

const x = {
    a: [1, 2]
};

assert.commandWorked(coll.insert({a: [[1, 2]]}));
assert.eq(1, coll.find(x).count());

assert.commandWorked(coll.insert(x));
assert.eq(2, coll.find(x).count());

assert.commandWorked(coll.createIndex({a: 1}));
assert.eq(2, coll.find(x).count());
}());
