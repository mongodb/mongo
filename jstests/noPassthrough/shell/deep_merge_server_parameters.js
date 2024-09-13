/**
 * Tests that ShardingTest parameters that are specified in the other field do not overwrite general
 * parameters. Also includes unit tests to test new deepMerge helper function.
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

// Basic test to ensure both parameters are appended in config options instead of replaced.
let st = new ShardingTest({
    configOptions: {
        setParameter: {
            reshardingCriticalSectionTimeoutMillis: 86400000,
        }
    },
    shards: 1,
    other: {
        configOptions: {
            setParameter: {
                logComponentVerbosity:
                    "{verbosity: 0, command: {verbosity: 1}, network: {verbosity: 1, asio: {verbosity: 2}}}",
            }
        }
    }
});
// Check that reshardingCriticalSectionTimeoutMillis is in config options with the proper value.
assert(st._otherParams.configOptions.setParameter.hasOwnProperty(
           "reshardingCriticalSectionTimeoutMillis"),
       "setParameter does not contain reshardingCriticalSectionTimeoutMillis");
assert.eq(st._otherParams.configOptions.setParameter.reshardingCriticalSectionTimeoutMillis,
          86400000,
          "The value of reshardingCriticalSectionTimeoutMillis is not correct");
// Check that logComponentVerbosity is in config options with the proper value.
assert(st._otherParams.configOptions.setParameter.hasOwnProperty("logComponentVerbosity"),
       "setParameter does not contain logComponentVerbosity");
assert.eq(st._otherParams.configOptions.setParameter.logComponentVerbosity,
          "{verbosity: 0, command: {verbosity: 1}, network: {verbosity: 1, asio: {verbosity: 2}}}",
          "The value of logComponentVerbosity is not correct");

st.stop();

// Additional unit tests for new deepMerge function

// deepMerge with empty object.
let emptyTest = Object.deepMerge({});
assert(Object.keys(emptyTest).length === 0);

// deepMerge with one empty object.
let oneArgumentTest = Object.deepMerge({}, {int: 1});
assert(Object.keys(oneArgumentTest).length === 1);
assert(oneArgumentTest.hasOwnProperty("int"));
assert.eq(oneArgumentTest.int, 1);

// deepMerge with matching keys and two array fields.
let arrayTest = Object.deepMerge({arr: [1, 2, 3]}, {arr: [4, 5, 6]});
assert(arrayTest.hasOwnProperty("arr"));
assert.eq(arrayTest.arr, [1, 2, 3, 4, 5, 6]);

// deepMerge with matching keys and two object fields.
let twoObjectTest = Object.deepMerge({nest: {one: 1}}, {nest: {two: 2}});
assert(twoObjectTest.hasOwnProperty("nest"));
assert(twoObjectTest.nest.hasOwnProperty("one"));
assert(twoObjectTest.nest.hasOwnProperty("two"));
assert.eq(twoObjectTest.nest.one, 1);
assert.eq(twoObjectTest.nest.two, 2);

// deepMerge with 3 objects, two of which have a matching key.
let obj1 = {a: {num: 123}};
let obj2 = {a: {letter: "a"}};
let obj3 = {b: "hello"};
let threeObjectTest = Object.deepMerge(obj1, obj2, obj3);
assert(threeObjectTest.hasOwnProperty("a"));
assert(threeObjectTest.hasOwnProperty("b"));
assert(threeObjectTest.a.hasOwnProperty("num"));
assert(threeObjectTest.a.hasOwnProperty("letter"));
assert.eq(threeObjectTest.a.num, 123);
assert.eq(threeObjectTest.a.letter, "a");
assert.eq(threeObjectTest.b, "hello");

// deepMerge with one undefined object.
let undefinedTest = Object.deepMerge(undefined);
assert.eq(Object.keys(undefinedTest).length, 0);

// deepMerge with two objects, one of which is undefined
let undefinedMergeTest = Object.deepMerge(undefined, {a: 1});
assert(undefinedMergeTest.hasOwnProperty("a"));
assert.eq(undefinedMergeTest.a, 1);
assert.eq(Object.keys(undefinedMergeTest).length, 1);

// deepMerge with two objects with a swapped order from the previous test.
let orderTest = Object.deepMerge({a: 1}, undefined);
assert(orderTest.hasOwnProperty("a"));
assert.eq(orderTest.a, 1);
assert.eq(Object.keys(orderTest).length, 1);

// deepMerge with two objects, one of which has a undefined value with a defined key.
let undefinedValueTest = Object.deepMerge({a: 1}, {b: undefined});
assert(undefinedValueTest.hasOwnProperty("a"));
assert(undefinedValueTest.hasOwnProperty("b"));
assert.eq(undefinedValueTest.a, 1);
assert.eq(undefinedValueTest.b, undefined);
