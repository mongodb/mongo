// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   sbe_incompatible,
// ]

// See SERVER-9448
// Test argument and receiver (aka 'this') objects and their children can be mutated
// in Map, Reduce and Finalize functions
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

const collection = db.mrMutableReceiver;
collection.drop();
collection.insert({a: 1});

const map = function() {
    // set property on receiver
    this.feed = {beef: 1};

    // modify property on receiever
    this.a = {cake: 1};
    emit(this._id, this.feed);
    emit(this._id, this.a);
};

const reduce = function(key, values) {
    // set property on receiver
    this.feed = {beat: 1};

    // set property on key arg
    key.fed = {mochi: 1};

    // push properties onto values array arg
    values.push(this.feed);
    values.push(key.fed);

    // modify each value in the (modified) array arg
    values.forEach(function(val) {
        val.mod = 1;
    });
    return {food: values};
};

const finalize = function(key, values) {
    // set property on receiver
    this.feed = {ice: 1};

    // set property on key arg
    key.fed = {cream: 1};

    // push properties onto values array arg
    printjson(values);
    values.food.push(this.feed);
    values.food.push(key.fed);

    // modify each value in the (modified) array arg
    values.food.forEach(function(val) {
        val.mod = 1;
    });
    return values;
};

const cmdResult = collection.mapReduce(map, reduce, {finalize: finalize, out: {inline: 1}});

assertArrayEq(cmdResult.results[0].value.food, [
    {"cake": 1, "mod": 1},
    {"beef": 1, "mod": 1},
    {"beat": 1, "mod": 1},
    {"mochi": 1, "mod": 1},
    {"ice": 1, "mod": 1},
    {"cream": 1, "mod": 1}
]);
}());
