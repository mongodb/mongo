/**
 * Tests that map reduce works with stored javascript.
 *
 * @tags: [
 *  # The test runs commands that are not allowed with security token: mapReduce.
 *  not_allowed_with_signed_security_token,
 *  # This test expects a function stored in the system.js collection to be available for a
 *  # map/reduce, which may not be the case if it is implicitly sharded in a passthrough.
 *  assumes_unsharded_collection,
 *  # mapReduce does not support afterClusterTime.
 *  does_not_support_causal_consistency,
 *  does_not_support_stepdowns,
 *  requires_non_retryable_writes,
 *  uses_map_reduce_with_temp_collections,
 *  requires_scripting,
 *  # TODO SERVER-82802: re-enable this test in suites with random migrations
 *  # "system.js" collection is not preserved when data is migrated
 *  # to other shards (e.g. moveRange, moveCollection)
 *  assumes_balancer_off,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";

// Use a unique database name to avoid conflicts with other tests that directly modify
// system.js.
const testDB = db.getSiblingDB("mr_stored");
const coll = testDB.test;
coll.drop();

assert.commandWorked(coll.insert({"partner": 1, "visits": 9}));
assert.commandWorked(coll.insert({"partner": 2, "visits": 9}));
assert.commandWorked(coll.insert({"partner": 1, "visits": 11}));
assert.commandWorked(coll.insert({"partner": 1, "visits": 30}));
assert.commandWorked(coll.insert({"partner": 2, "visits": 41}));
assert.commandWorked(coll.insert({"partner": 2, "visits": 41}));

let map = function(obj) {
    emit(obj.partner, {stats: [obj.visits]});
};

let notStoredMap = `function() {(${map.toString()})(this);}`;

const reduce = function(k, v) {
    var stats = [];
    var total = 0;
    for (var i = 0; i < v.length; i++) {
        for (var j in v[i].stats) {
            stats.push(v[i].stats[j]);
            total += v[i].stats[j];
        }
    }
    return {stats: stats, total: total};
};

const finalize = function(key, reducedValue) {
    reducedValue.avg = reducedValue.total / reducedValue.stats.length;
    return reducedValue;
};

const systemJsColl = testDB.getCollection("system.js");
assert.commandWorked(systemJsColl.insert({_id: "mr_stored_map", value: map}));
assert.commandWorked(systemJsColl.insert({_id: "mr_stored_reduce", value: reduce}));
assert.commandWorked(systemJsColl.insert({_id: "mr_stored_finalize", value: finalize}));

const out = testDB.mr_stored_out;

function assertCorrect(results) {
    assert.eq(2, Object.keySet(results).length);
    assertArrayEq({actual: [9, 11, 30], expected: results["1"].stats});
    assertArrayEq({actual: [9, 41, 41], expected: results["2"].stats});
}

// Stored Map.
assert.commandWorked(testDB.runCommand({
    mapReduce: coll.getName(),
    map: function() {
        // eslint-disable-next-line
        mr_stored_map(this);
    },
    reduce: reduce,
    finalize: finalize,
    out: "mr_stored_out"
}));

assertCorrect(out.convertToSingleObject("value"));
out.drop();

// Stored Reduce.
assert.commandWorked(testDB.runCommand({
    mapReduce: coll.getName(),
    map: notStoredMap,
    reduce: function(k, v) {
        // eslint-disable-next-line
        return mr_stored_reduce(k, v);
    },
    finalize: finalize,
    out: "mr_stored_out"
}));

assertCorrect(out.convertToSingleObject("value"));
out.drop();

// Stored Finalize.
assert.commandWorked(testDB.runCommand({
    mapReduce: coll.getName(),
    map: notStoredMap,
    reduce: reduce,
    finalize: function(key, reducedValue) {
        // eslint-disable-next-line
        return mr_stored_finalize(key, reducedValue);
    },
    out: "mr_stored_out"
}));

assertCorrect(out.convertToSingleObject("value"));
out.drop();

// All Stored.
assert.commandWorked(testDB.runCommand({
    mapReduce: coll.getName(),
    map: function() {
        // eslint-disable-next-line
        mr_stored_map(this);
    },
    reduce: function(k, v) {
        // eslint-disable-next-line
        return mr_stored_reduce(k, v);
    },
    finalize: function(key, reducedValue) {
        // eslint-disable-next-line
        return mr_stored_finalize(key, reducedValue);
    },
    out: "mr_stored_out"
}));

assertCorrect(out.convertToSingleObject("value"));
out.drop();

assert.commandWorked(systemJsColl.remove({_id: "mr_stored_map"}));
assert.commandWorked(systemJsColl.remove({_id: "mr_stored_reduce"}));
assert.commandWorked(systemJsColl.remove({_id: "mr_stored_finalize"}));
