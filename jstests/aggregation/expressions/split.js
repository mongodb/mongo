// In SERVER-6773, the $split expression was introduced. In this file, we test the functionality and
// error cases of the expression.
/**
 * @tags: [
 *   featureFlagMqlJsEngineGap,
 *   requires_fcv_82
 * ]
 */

import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode, testExpression} from "jstests/aggregation/extras/utils.js";

const coll = db.split;
coll.drop();
assert.commandWorked(coll.insert({}));

function runAndAssert(args, result) {
    // Test with constant-folding optimization.
    testExpression(coll, {$split: args}, result);
    coll.drop();

    // Insert args as document.
    const document = {};
    if (args[0] != "$missing") {
        document.input = args[0];
    }
    if (args[1] != "$missing") {
        document.delimiter = args[1];
    }
    assert.commandWorked(coll.insertOne(document));

    // Test again with fields from document.
    assert.eq(coll.aggregate([{$project: {result: {$split: ["$input", "$delimiter"]}}}])
                  .toArray()[0]
                  .result,
              result);

    // Clean up.
    coll.drop();
    assert.commandWorked(coll.insertOne({}));
}

// Basic tests.
runAndAssert(["abc", "b"], ["a", "c"]);
runAndAssert(["aaa", "b"], ["aaa"]);
runAndAssert(["a b a", "b"], ["a ", " a"]);
runAndAssert(["a", "a"], ["", ""]);
runAndAssert(["aa", "a"], ["", "", ""]);
runAndAssert(["aaa", "a"], ["", "", "", ""]);
runAndAssert(["", "a"], [""]);
runAndAssert(["abc abc cba abc", "abc"], ["", " ", " cba ", ""]);
runAndAssert(["abc", /b/], ["a", "c"]);
runAndAssert(["aaa", /b/], ["aaa"]);
runAndAssert(["a b a", /b/], ["a ", " a"]);
runAndAssert(["a", /a/], ["", ""]);
runAndAssert(["aa", /a/], ["", "", ""]);
runAndAssert(["aaa", /a/], ["", "", "", ""]);
runAndAssert(["", /a/], [""]);
runAndAssert(["abc abc cba abc", /abc/], ["", " ", " cba ", ""]);

// Ensure that $split operates correctly when the string has embedded null bytes.
runAndAssert(["a\0b\0c", "\0"], ["a", "b", "c"]);
runAndAssert(["\0a\0", "a"], ["\0", "\0"]);
runAndAssert(["\0\0\0", "a"], ["\0\0\0"]);
runAndAssert(["a\0b\0c", /\0/], ["a", "b", "c"]);
runAndAssert(["\0a\0", /a/], ["\0", "\0"]);
runAndAssert(["\0\0\0", /a/], ["\0\0\0"]);

// Ensure that $split operates correctly when the string has multi-byte tokens or input strings.
runAndAssert(["∫a∫", "a"], ["∫", "∫"]);
runAndAssert(["a∫∫a", "∫"], ["a", "", "a"]);
runAndAssert(["∫a∫", /a/], ["∫", "∫"]);
runAndAssert(["a∫∫a", /∫/], ["a", "", "a"]);

// Ensure that $split produces null when given null as input.
runAndAssert(["abc", null], null);
runAndAssert([null, "abc"], null);
runAndAssert([null, /abc/], null);

// Ensure that $split produces null when given missing fields as input.
runAndAssert(["$missing", "a"], null);
runAndAssert(["a", "$missing"], null);
runAndAssert(["$missing", "$missing"], null);
runAndAssert(["$missing", /a/], null);

// Complex Matching with Captures.
runAndAssert(["abacd", /(a)(b)/], ["", "a", "b", "acd"]);
runAndAssert(["abacd", /(a)(b)?(c)?/], ["", "a", "b", "", "", "a", "", "c", "d"]);
runAndAssert(["xyz", /((x))/], ["", "x", "x", "yz"]);
runAndAssert(["xyz", /((x)*)/], ["", "x", "x", "", "", "", "y", "", "", "z", "", "", ""]);

// Zero-width matches.
runAndAssert(["abc", /(?=b)/], ["a", "bc"]);
runAndAssert(["abc", /(?<=b)/], ["ab", "c"]);
runAndAssert(["abc", /(?=c)|(?<=c)/], ["ab", "c", ""]);
runAndAssert(["xyz", /(?:)/], ["", "x", "y", "z", ""]);

// Special Character Classes.
runAndAssert(["a1b2c3", /[0-9]/], ["a", "b", "c", ""]);
runAndAssert(["The quick brown fox.", /\s+/], ["The", "quick", "brown", "fox."]);
runAndAssert(["test-123-data", /[-]/], ["test", "123", "data"]);

// Escape Sequences.
runAndAssert(["a*b*c", /\*/], ["a", "b", "c"]);
runAndAssert(["a.b.c", /\./], ["a", "b", "c"]);

// Anchors.
runAndAssert(["abc", /^a/], ["", "bc"]);
runAndAssert(["abc", /c$/], ["ab", ""]);
runAndAssert(["abc", /^$/], ["abc"]);

// Multi-line Strings.
runAndAssert(["line1\nline2\nline3", /\n/], ["line1", "line2", "line3"]);

// Unicode and Extended Character Classes.
runAndAssert(["𝌆a𝌆b𝌆c", /𝌆/], ["", "a", "b", "c"]);
runAndAssert(["♠♦♣", /[♠♦♣]/], ["", "", "", ""]);
runAndAssert(["∫∫∫abc∫∫∫", /∫+/], ["", "abc", ""]);

// Regex Combining Multiple Features.
runAndAssert(["abc-def_ghi", /[-_]/], ["abc", "def", "ghi"]);
runAndAssert(["a123.456b", /([0-9]+)(\.)/], ["a", "123", ".", "456b"]);

//
// Error Code tests with constant-folding optimization.
//

// Ensure that $split errors when given more or less than two arguments.
let pipeline = {$project: {split: {$split: []}}};
assertErrorCode(coll, pipeline, 16020);

pipeline = {
    $project: {split: {$split: ["a"]}}
};
assertErrorCode(coll, pipeline, 16020);

pipeline = {
    $project: {split: {$split: ["a", "b", "c"]}}
};
assertErrorCode(coll, pipeline, 16020);

// Ensure that $split errors when given non-string/regex input.
pipeline = {
    $project: {split: {$split: [1, "abc"]}}
};
assertErrorCode(coll, pipeline, 40085);

pipeline = {
    $project: {split: {$split: [1, /abc/]}}
};
assertErrorCode(coll, pipeline, 40085);

pipeline = {
    $project: {split: {$split: ["abc", 1]}}
};
assertErrorCode(coll, pipeline, 40086);

// Ensure that $split errors when given an empty separator.
pipeline = {
    $project: {split: {$split: ["abc", ""]}}
};
assertErrorCode(coll, pipeline, 40087);
