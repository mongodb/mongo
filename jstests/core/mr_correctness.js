// Basic correctness tests for the mapReduce command.
// @tags: [
//   # mapReduce does not support afterClusterTime.
//   does_not_support_causal_consistency,
//   does_not_support_stepdowns,
//   requires_fastcount,
//   requires_getmore,
//   uses_map_reduce_with_temp_collections,
// ]
(function() {
"use strict";
load("jstests/libs/fixture_helpers.js");  // For "FixtureHelpers".

const coll = db.mr_correctness;
coll.drop();

assert.commandWorked(coll.insert({x: 1, tags: ["a", "b"]}));
assert.commandWorked(coll.insert({x: 2, tags: ["b", "c"]}));
assert.commandWorked(coll.insert({x: 3, tags: ["c", "a"]}));
assert.commandWorked(coll.insert({x: 4, tags: ["b", "c"]}));

function mapToObj() {
    this.tags.forEach(function(z) {
        emit(z, {count: 1});
    });
}

function reduceObjs(key, values) {
    let total = 0;
    for (let i = 0; i < values.length; i++) {
        total += values[i].count;
    }
    return {count: total};
}

const outColl = db[coll.getName() + "_out"];
outColl.drop();
(function testBasicMapReduce() {
    const res = db.runCommand(
        {mapReduce: coll.getName(), map: mapToObj, reduce: reduceObjs, out: outColl.getName()});
    assert.commandWorked(res);
    assert.eq(4, res.counts.input);
    assert.eq(res.result, outColl.getName());

    assert.eq(
        3,
        outColl.find().count(),
        () =>
            `expected 3 distinct tags: ['a', 'b', 'c'], found ${tojson(outColl.find().toArray())}`);
    const keys = {};
    for (let result of outColl.find().toArray()) {
        keys[result._id] = result.value.count;
    }
    assert.eq(3, Object.keySet(keys).length, Object.keySet(keys));
    assert.eq(2, keys.a, () => `Expected 2 occurences of the tag 'a': ${tojson(keys)}`);
    assert.eq(3, keys.b, () => `Expected 3 occurences of the tag 'b': ${tojson(keys)}`);
    assert.eq(3, keys.c, () => `Expected 3 occurences of the tag 'c': ${tojson(keys)}`);
    outColl.drop();
})();

(function testMapReduceWithPredicate() {
    const res = db.runCommand({
        mapReduce: coll.getName(),
        map: mapToObj,
        reduce: reduceObjs,
        query: {x: {$gt: 2}},
        out: outColl.getName()
    });
    assert.commandWorked(res);
    assert.eq(2, res.counts.input, () => tojson(res));
    assert.eq(res.result, outColl.getName());
    const keys = {};
    for (let result of outColl.find().toArray()) {
        keys[result._id] = result.value.count;
    }
    assert.eq(3, Object.keySet(keys).length, Object.keySet(keys));
    assert.eq(1, keys.a, () => `Expected 1 occurence of the tag 'a': ${tojson(keys)}`);
    assert.eq(1, keys.b, () => `Expected 1 occurence of the tag 'b': ${tojson(keys)}`);
    assert.eq(2, keys.c, () => `Expected 2 occurences of the tag 'c': ${tojson(keys)}`);
    outColl.drop();
}());

function mapToNumber() {
    for (let tag of this.tags) {
        emit(tag, 1);
    }
}

function reduceNumbers(key, values) {
    let total = 0;
    for (let val of values) {
        total += val;
    }
    return total;
}

// Now do a similar test but using the above map and reduce functions which use numbers as the value
// instead of objects.
(function testBasicMapReduceWithNumberValues() {
    const res = db.runCommand({
        mapReduce: coll.getName(),
        map: mapToNumber,
        reduce: reduceNumbers,
        query: {x: {$gt: 2}},
        out: outColl.getName()
    });
    assert.commandWorked(res);
    assert.eq(2, res.counts.input, () => tojson(res));
    assert.eq(res.result, outColl.getName());
    const keys = {};
    for (let result of outColl.find().toArray()) {
        keys[result._id] = result.value;
    }
    assert.eq(3, Object.keySet(keys).length, Object.keySet(keys));
    assert.eq(1, keys.a, () => `Expected 1 occurence of the tag 'a': ${tojson(keys)}`);
    assert.eq(1, keys.b, () => `Expected 1 occurence of the tag 'b': ${tojson(keys)}`);
    assert.eq(2, keys.c, () => `Expected 2 occurences of the tag 'c': ${tojson(keys)}`);
    outColl.drop();
}());

(function testMapReduceWithManyValuesGrouped() {
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 5; i < 1000; i++) {
        bulk.insert({x: i, tags: ["b", "d"]});
    }
    assert.commandWorked(bulk.execute());

    const res = db.runCommand(
        {mapReduce: coll.getName(), map: mapToObj, reduce: reduceObjs, out: outColl.getName()});
    assert.commandWorked(res);
    assert.eq(999, res.counts.input, () => tojson(res));
    assert.eq(res.result, outColl.getName());
    assert.eq(4,
              outColl.find().count(),
              () => `expected 4 distinct tags: ['a', 'b', 'c', 'd'], found ${
                  tojson(outColl.find().toArray())}`);
    assert.eq("a,b,c,d", outColl.distinct("_id"));

    assert.eq(2, outColl.findOne({_id: "a"}).value.count, () => outColl.findOne({_id: "a"}));
    assert.eq(998, outColl.findOne({_id: "b"}).value.count, () => outColl.findOne({_id: "b"}));
    assert.eq(3, outColl.findOne({_id: "c"}).value.count, () => outColl.findOne({_id: "c"}));
    assert.eq(995, outColl.findOne({_id: "d"}).value.count, () => outColl.findOne({_id: "d"}));
    outColl.drop();
}());

(function testThatVerboseOptionIncludesTimingInformation() {
    const cmd =
        {mapReduce: coll.getName(), map: mapToObj, reduce: reduceObjs, out: outColl.getName()};
    const withoutVerbose = assert.commandWorked(db.runCommand(cmd));
    // TODO SERVER-43290 The verbose option should have the same effect on mongos.
    assert(FixtureHelpers.isMongos(db) || !withoutVerbose.hasOwnProperty("timing"));
    const withVerbose = assert.commandWorked(db.runCommand(Object.merge(cmd, {verbose: true})));
    assert(withVerbose.hasOwnProperty("timing"));
}());

(function testMapReduceAgainstNonExistentCollection() {
    assert.commandFailedWithCode(db.runCommand({
        mapReduce: "lasjdlasjdlasjdjasldjalsdj12e",
        map: mapToObj,
        reduce: reduceObjs,
        out: outColl.getName()
    }),
                                 ErrorCodes.NamespaceNotFound);
}());

(function testHighCardinalityKeySet() {
    let correctValues = {};

    coll.drop();
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < 20000; i++) {
        const tag = "Z" + i % 10000;
        if (!correctValues[tag])
            correctValues[tag] = 1;
        else
            correctValues[tag]++;
        bulk.insert({x: i, tags: [tag]});
    }
    assert.commandWorked(bulk.execute());

    const res = db.runCommand(
        {mapReduce: coll.getName(), out: outColl.getName(), map: mapToObj, reduce: reduceObjs});
    assert.commandWorked(res);
    assert.eq(res.result, outColl.getName());
    let actualValues = {};
    outColl.find().forEach(function(resultDoc) {
        actualValues[resultDoc._id] = resultDoc.value.count;
    });
    for (let key in actualValues) {
        assert.eq(correctValues[key], actualValues[key], key);
    }
}());

coll.drop();
outColl.drop();
}());
