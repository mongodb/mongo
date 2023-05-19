// See SERVER-9448
// Test argument and receiver (aka 'this') objects and their children can be mutated
// in Map, Reduce and Finalize functions
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

const map = function() {
    // set property on receiver
    this.feed = {beef: 1};

    // modify property on receiever
    this.a = {cake: 1};
    emit(this._id, this.feed);
    emit(this._id, this.a);
};

const reduce = function(key, values) {
    // Deal with the possibility that the input 'values' may have already been partially reduced.
    values = values.reduce(function(acc, current) {
        if (current.hasOwnProperty("food")) {
            return acc.concat(current.food);
        } else {
            acc.push(current);
            return acc;
        }
    }, []);

    // Set property on receiver.
    this.feed = {beat: 1};

    // Set property on key arg.
    key.fed = {mochi: 1};

    // Push properties onto values array arg, if they are not present in the array already due to
    // an earlier reduction.
    if (!values.some(obj => obj.hasOwnProperty("beat"))) {
        values.push(this.feed);
    }
    if (!values.some(obj => obj.hasOwnProperty("mochi"))) {
        values.push(key.fed);
    }

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

const runTest = function(coll) {
    coll.drop();
    coll.insert({a: 1});

    const cmdResult = coll.mapReduce(map, reduce, {finalize: finalize, out: {inline: 1}});

    assertArrayEq({
        actual: cmdResult.results[0].value.food,
        expected: [
            {"cake": 1, "mod": 1},
            {"beef": 1, "mod": 1},
            {"beat": 1, "mod": 1},
            {"mochi": 1, "mod": 1},
            {"ice": 1, "mod": 1},
            {"cream": 1, "mod": 1}
        ]
    });
};

let conn = MongoRunner.runMongod({setParameter: {mrEnableSingleReduceOptimization: true}});
assert.neq(null, conn, "mongod was unable to start up");
runTest(conn.getDB("test").mrMutableReceiver);
MongoRunner.stopMongod(conn);

let st = new ShardingTest({shards: 2, setParameter: {mrEnableSingleReduceOptimization: true}});
assert.neq(null, st.s, "mongod was unable to start up");
st.s.adminCommand({shardCollection: "test.mrMutableReceiver"});
runTest(st.s.getDB("test").mrMutableReceiver);
st.stop();
}());
