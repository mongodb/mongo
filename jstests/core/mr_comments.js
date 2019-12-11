// Tests that multi-line strings with comments are allowed within mapReduce.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

const coll = db.mr_comments;
coll.drop();
const outColl = db.mr_comments_out;
outColl.drop();

assert.commandWorked(coll.insert([{foo: 1}, {foo: 1}, {foo: 2}]));

// Test using comments within the function.
assert.commandWorked(db.runCommand({
    mapreduce: coll.getName(),
    map: "// This is a comment\n\n    // Emit some stuff\n    emit(this.foo, 1)\n",
    reduce: function(key, values) {
        return Array.sum(values);
    },
    out: {merge: outColl.getName()}
}));
assert.eq(2, outColl.find().toArray().length);

// Test using a multi-line string literal.
outColl.drop();
assert.commandWorked(db.runCommand({
    mapreduce: coll.getName(),
    map: `
    // This is a comment

    // Emit some stuff
    emit(this.foo, 1);
    `,
    reduce: function(key, values) {
        return Array.sum(values);
    },
    out: {merge: outColl.getName()}
}));
assert.eq(2, outColl.find().toArray().length);

// Test that a function passed with a comment in front of it is still recognized.
outColl.drop();
assert.commandWorked(db.runCommand({
    mapreduce: coll.getName(),
    map: "// This is a comment\nfunction(){\n    // Emit some stuff\n    emit(this.foo, 1)\n}\n",
    reduce: function(key, values) {
        return Array.sum(values);
    },
    out: {merge: outColl.getName()}
}));
assert.eq(2, outColl.find().toArray().length);
}());
