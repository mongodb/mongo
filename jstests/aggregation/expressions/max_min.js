/*
 * Tests $max and $min when used as aggregation expressions.
 */

var coll = db.collection;
assert(coll.drop());
coll.insertOne({
    "int1": 5,
    "int2": 10,
    "str1": "hello",
    "str2": "hello world",
    "NaN": NaN,
    "arr1": [5],
    "arr2": [1, 2, 3],
    "arr3": [1, 2, "string", null],
    "arr4": [],
    "arrNest1": [[1, 2]],
    "arrNest2": [[1, 2], [3, 4]],
    "arrNest3": [[[1, 2, 3]]],
    "arrNest4": [[[1, 2, 3]], 1],
    "arrMixed1": [[], "hello", ["c"]],
    "arrMixed2": [[], "hello", ["c"], null],
    "null": null,
    "undefined1": [undefined],
    "undefined2": [undefined, 2, "string"]
});

function assertResultMax(expected, params) {
    assert.eq(expected, coll.aggregate({$project: {a: {$max: params}}}).toArray()[0].a);
}

function assertResultMin(expected, params) {
    assert.eq(expected, coll.aggregate({$project: {a: {$min: params}}}).toArray()[0].a);
}

// Test single parameter.
assertResultMax(null, []);
assertResultMax(null, "$arr4");
assertResultMax(5, "$arr1");
assertResultMax(5, "$int1");
assertResultMax("hello", "$str1");
assertResultMax(NaN, "$NaN");
assertResultMax(null, "$null");

assertResultMin(null, []);
assertResultMin(null, "$arr4");
assertResultMin(5, "$arr1");
assertResultMin(5, "$int1");
assertResultMin("hello", "$str1");
assertResultMin(NaN, "$NaN");
assertResultMin(null, "$null");

// Test multiple parameters.
assertResultMax(10, ["$int2", "$int1"]);
assertResultMax("hello", ["$int2", "$int1", "$str1"]);
assertResultMax("hello world", ["$str1", "$str2"]);

assertResultMin(5, ["$int2", "$int1"]);
assertResultMin(5, ["$int2", "$int1", "$str1"]);
assertResultMin("hello", ["$str1", "$str2"]);

// Tests single array param.
assertResultMax(null, "$undefined1");
assertResultMax("string", "$undefined2");
assertResultMax(3, "$arr2");
assertResultMax("string", "$arr3");

assertResultMin(null, "$undefined1");
assertResultMin(2, "$undefined2");
assertResultMin(1, "$arr2");
assertResultMin(1, "$arr3");

// Test nested arrays.
assertResultMax([1, 2], "$arrNest1");
assertResultMax([3, 4], "$arrNest2");
assertResultMax([[1, 2, 3]], "$arrNest3");
assertResultMax([1, 2, 3], ["$arr2", []]);
assertResultMax([[1, 2, 3]], "$arrNest4");
assertResultMax(["c"], "$arrMixed1");
assertResultMax([[], "hello", ["c"], null], ["$arrMixed1", "$arrMixed2"]);

assertResultMin([1, 2], "$arrNest1");
assertResultMin([1, 2], "$arrNest2");
assertResultMin([[1, 2, 3]], "$arrNest3");
assertResultMin([], ["$arr2", []]);
assertResultMin(1, "$arrNest4");
assertResultMin("hello", "$arrMixed1");
assertResultMin([[], "hello", ["c"]], ["$arrMixed1", "$arrMixed2"]);

// Multiple null or undefined params
assertResultMax(null, [null, "$null", "$doesNotExist"], undefined);

assertResultMin(null, [null, "$null", "$doesNotExist"], undefined);

assert(coll.drop());
