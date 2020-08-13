// Tests that mapReduce fails gracefully when given a map or reduce function which fails in some
// way.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   sbe_incompatible,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";

const coll = db.mr_fail_invalid_js;
const outputColl = db.mr_fail_invalid_js_out;

// Test that a map or reduce function which references a path which doesn't exist fails gracefully.
(function testReferencingInvalidPaths() {
    coll.drop();
    outputColl.drop();

    assert.commandWorked(coll.insert([
        {x: 1, tags: ["a", "b"]},
        {x: 2, tags: ["b", "c"]},
        {x: 3, tags: ["c", "a"]},
        {x: 4, tags: ["b", "c"]}
    ]));

    let reduceFn = function(key, values) {
        return Array.sum(values);
    };

    const goodMapFn = function() {
        for (let tag of this.tags) {
            emit(tag, 1);
        }
    };
    assert.commandWorked(coll.mapReduce(goodMapFn, reduceFn, {out: {merge: outputColl.getName()}}));
    outputColl.drop();

    // mapReduce fails when attempting to merge a missing key.
    const singleInvalidPathMapFn = function() {
        emit(this.missing_field, this.x);
    };

    assert.throws(() => coll.mapReduce(
                      singleInvalidPathMapFn, reduceFn, {out: {merge: outputColl.getName()}}),
                  []);

    // Now test that a traversal through a missing path will cause an error.
    const badMapFn = function() {
        emit(this._id, this.missing_field.nested_missing);
    };

    assert.throws(
        () => coll.mapReduce(newMapFn, reduceFn, {out: {merge: outputColl.getName()}}),
        [],
        "expected mapReduce to throw because map function references path that does not exist");

    // Test the same thing but in the reduce function.
    reduceFn = function(k, v) {
        return v.missing_field.increasingly_missing.all_hope_is_lost;
    };
    assert.throws(
        () => coll.mapReduce(goodMapFn, reduceFn, outputColl.getName()),
        [],
        "expected mapReduce to throw because reduce function references path that does not exist");
}());

// Test that a map function which supplies the wrong number of arguments to 'emit' will fail
// gracefully.
(function testBadCallToEmit() {
    coll.drop();
    outputColl.drop();
    assert.commandWorked(coll.insert([{a: [1, 2, 3]}, {a: [2, 3, 4]}]));
    const goodMapFn = function() {
        for (let i = 0; i < this.a.length; i++) {
            emit(this.a[i], 1);
        }
    };

    const goodReduceFn = function(k, v) {
        let total = 0;
        for (let i = 0; i < v.length; i++)
            total += v[i];
        return total;
    };

    // First test that a valid command succeeds.
    let res = coll.mapReduce(goodMapFn, goodReduceFn, {out: {merge: outputColl.getName()}});

    assert.eq([{_id: 1, value: 1}, {_id: 2, value: 2}, {_id: 3, value: 2}, {_id: 4, value: 1}],
              outputColl.find().sort({_id: 1}).toArray());
    assert(outputColl.drop());

    const badMapFn = function() {
        for (let i = 0; i < this.a.length; i++) {
            emit(this.a[i]);
        }
    };

    const error = assert.commandFailed(db.runCommand({
        mapReduce: coll.getName(),
        map: badMapFn,
        reduce: goodReduceFn,
        out: outputColl.getName()
    }));
    assert(error.errmsg.indexOf("emit") >= 0, () => tojson(error));

    // Test that things are still in an ok state and the next mapReduce can succeed.
    outputColl.drop();
    assert.commandWorked(
        coll.mapReduce(goodMapFn, goodReduceFn, {out: {merge: outputColl.getName()}}));
    assert.eq([{_id: 1, value: 1}, {_id: 2, value: 2}, {_id: 3, value: 2}, {_id: 4, value: 1}],
              outputColl.find().sort({_id: 1}).toArray());
    assert(outputColl.drop());
}());
}());
