// Test $replaceOne aggregation expressions.
/**
 * @tags: [
 *   featureFlagMqlJsEngineGap,
 *   requires_fcv_82
 * ]
 */

import "jstests/libs/query/sbe_assert_error_override.js";

import {testExpression} from "jstests/aggregation/extras/utils.js";

const coll = db.replace_all;
coll.drop();
assert.commandWorked(coll.insert({}));

function runAndAssert(args, result) {
    // Test with constant-folding optimization.
    testExpression(coll, {$replaceAll: args}, result);
    coll.drop();

    // Insert args as document.
    const document = {};
    if (args.input != "$missing") {
        document.input = args.input;
    }
    if (args.find != "$missing") {
        document.find = args.find;
    }
    if (args.replacement != "$missing") {
        document.replacement = args.replacement;
    }
    assert.commandWorked(coll.insertOne(document));

    // Test again with fields from document.
    assert.eq(
        coll.aggregate([{
                $project: {
                    result:
                        {$replaceAll: {input: "$input", find: "$find", replacement: "$replacement"}}
                }
            }])
            .toArray()[0]
            .result,
        result);

    // Clean up.
    coll.drop();
    assert.commandWorked(coll.insertOne({}));
}

function runAndAssertThrows(args, code) {
    const error = assert.throws(
        () => coll.aggregate([{$project: {args, result: {$replaceAll: args}}}]).toArray());
    assert.commandFailedWithCode(error, code);
}

// Test find one.
runAndAssert({input: "albatross", find: "ross", replacement: "rachel"}, "albatrachel");
runAndAssert({input: "albatross", find: "", replacement: "one "},
             "one aone lone bone aone tone rone oone sone sone ");
runAndAssert({input: "albatross", find: "", replacement: ""}, "albatross");
runAndAssert({input: "", find: "", replacement: "foo"}, "foo");
runAndAssert({input: "", find: "", replacement: ""}, "");

// Test find none.
runAndAssert({input: "albatross", find: "rachel", replacement: "ross"}, "albatross");
runAndAssert({input: "", find: "rachel", replacement: "ross"}, "");
runAndAssert({input: "", find: "rachel", replacement: ""}, "");

// Test finding many only replaces first occurrence.
runAndAssert({input: "an antelope is an animal", find: "an", replacement: ""}, " telope is  imal");
runAndAssert({input: "an antelope is an animal", find: "an", replacement: "this"},
             "this thistelope is this thisimal");

// Test that any combination of null and non-null arguments returns null.
runAndAssert({input: null, find: null, replacement: null}, null);
runAndAssert({input: "a", find: null, replacement: null}, null);
runAndAssert({input: null, find: "b", replacement: null}, null);
runAndAssert({input: null, find: null, replacement: "c"}, null);
runAndAssert({input: "a", find: "b", replacement: null}, null);
runAndAssert({input: null, find: "b", replacement: "c"}, null);
runAndAssert({input: "a", find: "b", replacement: null}, null);

// Test that combinations of missing and non-missing arguments returns null.
runAndAssert({input: "$missing", find: "$missing", replacement: "$missing"}, null);
runAndAssert({input: "a", find: "$missing", replacement: "$missing"}, null);
runAndAssert({input: "$missing", find: "b", replacement: "$missing"}, null);
runAndAssert({input: "$missing", find: "$missing", replacement: "c"}, null);
runAndAssert({input: "a", find: "b", replacement: "$missing"}, null);
runAndAssert({input: "a", find: "$missing", replacement: "c"}, null);
runAndAssert({input: "$missing", find: "b", replacement: "c"}, null);
runAndAssert({input: null, find: null, replacement: "$missing"}, null);
runAndAssert({input: null, find: "$missing", replacement: null}, null);
runAndAssert({input: "$missing", find: null, replacement: null}, null);

// Basic regex replacement.
runAndAssert({input: "123-456-7890", find: /\d{3}/, replacement: "xxx"}, "xxx-xxx-xxx0");
runAndAssert({input: "hello1world2", find: /\d/, replacement: "X"}, "helloXworldX");

// No match.
runAndAssert({input: "hello world", find: /xyz/, replacement: "abc"}, "hello world");

// Empty input and pattern.
runAndAssert({input: "", find: /.*/, replacement: "replaced"}, "replaced");

// No match.
runAndAssert({input: "line1\nline2", find: /^line/m, replacement: "start: "}, "start: 1\nstart: 2");

// Empty input and pattern.
runAndAssert({input: "FooBar", find: /foobar/i, replacement: "MATCHED"}, "MATCHED");

// RegEx groupings.
runAndAssert({input: "helloworld", find: /([aeiou]+)/, replacement: "X"}, "hXllXwXrld");
runAndAssert({input: "123-456-7890", find: /(\d{3})/, replacement: "xxx"}, "xxx-xxx-xxx0");
runAndAssert({input: "123.456.7890", find: /([0-9]+)(\.)/, replacement: "x"}, "xx7890");
runAndAssert({input: "helloworld", find: /(([aeiou]+)l)/, replacement: "X"}, "hXloworld");
//
// Reset and test that if any input is not a string, replaceOne fails with an error.
//

assert(coll.drop());
assert.commandWorked(coll.insertOne({
    obj_field: {a: 1, b: 1, c: {d: 2}},
    arr_field1: [1, 2, 3, "c"],
    arr_field2: ["aaaaa"],
    int_field: 1,
    dbl_field: 1.0,
    null_field: null,
    str_field: "foo",
    regex_field: /.*/,
}));

// replacement is not a string
const invalidReplacementCode = 10503902;
// find is not a string or regex
const invalidFindCode = 10503903;
// input is not a string
const invalidInputCode = 10503904;

runAndAssertThrows({input: "$obj_field", find: "$str_field", replacement: "$str_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$arr_field1", find: "$str_field", replacement: "$str_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$int_field", find: "$regex_field", replacement: "$str_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$dbl_field", find: "$regex_field", replacement: "$str_field"},
                   invalidInputCode);

runAndAssertThrows({input: "$str_field", find: "$obj_field", replacement: "$str_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$str_field", find: "$arr_field1", replacement: "$str_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$str_field", find: "$int_field", replacement: "$str_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$str_field", find: "$dbl_field", replacement: "$str_field"},
                   invalidFindCode);

runAndAssertThrows({input: "$str_field", find: "$str_field", replacement: "$obj_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$str_field", find: "$str_field", replacement: "$arr_field1"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$str_field", find: "$regex_field", replacement: "$int_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$str_field", find: "$regex_field", replacement: "$dbl_field"},
                   invalidReplacementCode);

runAndAssertThrows({input: "$str_field", find: "$arr_field2", replacement: "$dbl_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$obj_field", find: "$arr_field2", replacement: "$str_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$int_field", find: "$arr_field2", replacement: "$dbl_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$arr_field2", find: "$arr_field2", replacement: "$arr_field2"},
                   invalidInputCode);

//
// Test always throws when invalid fields are given, even if some fields are also null or missing.
//

runAndAssertThrows({input: "$obj_field", find: "$null_field", replacement: "$str_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$obj_field", find: "$missing_field", replacement: "$str_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$obj_field", find: "$str_field", replacement: "$null_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$obj_field", find: "$str_field", replacement: "$missing_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$obj_field", find: "$missing_field", replacement: "$null_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$obj_field", find: "$null_field", replacement: "$missing_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$obj_field", find: "$missing_field", replacement: "$missing_field"},
                   invalidInputCode);
runAndAssertThrows({input: "$obj_field", find: "$null_field", replacement: "$null_field"},
                   invalidInputCode);

runAndAssertThrows({input: "$null_field", find: "$obj_field", replacement: "$str_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$missing_field", find: "$obj_field", replacement: "$str_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$str_field", find: "$obj_field", replacement: "$null_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$str_field", find: "$obj_field", replacement: "$missing_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$missing_field", find: "$obj_field", replacement: "$null_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$null_field", find: "$obj_field", replacement: "$missing_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$missing_field", find: "$obj_field", replacement: "$missing_field"},
                   invalidFindCode);
runAndAssertThrows({input: "$null_field", find: "$obj_field", replacement: "$null_field"},
                   invalidFindCode);

runAndAssertThrows({input: "$null_field", find: "$str_field", replacement: "$obj_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$missing_field", find: "$str_field", replacement: "$obj_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$str_field", find: "$null_field", replacement: "$obj_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$str_field", find: "$missing_field", replacement: "$obj_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$missing_field", find: "$null_field", replacement: "$obj_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$null_field", find: "$missing_field", replacement: "$obj_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$missing_field", find: "$missing_field", replacement: "$obj_field"},
                   invalidReplacementCode);
runAndAssertThrows({input: "$null_field", find: "$null_field", replacement: "$obj_field"},
                   invalidReplacementCode);
