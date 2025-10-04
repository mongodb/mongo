// Aggregation $substrBytes tests.
import "jstests/libs/query/sbe_assert_error_override.js";

import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

let t = db.jstests_aggregation_substr;
t.drop();

t.save({});

function assertSubstring(expected, str, offset, len) {
    assert.eq(expected, t.aggregate({$project: {a: {$substrBytes: [str, offset, len]}}}).toArray()[0].a);
}

function assertArgsException(args) {
    assert.commandFailed(t.runCommand("aggregate", {pipeline: [{$substrBytes: args}]}));
}

function assertException(str, offset, len) {
    assertArgsException([str, offset, len]);
}

// Wrong number of arguments.
assertArgsException([]);
assertArgsException(["foo"]);
assertArgsException(["foo", 1]);
assertArgsException(["foo", 1, 1, 1]);

// Basic offset / length checks.
assertSubstring("abcd", "abcd", 0, 4);
assertSubstring("abcd", "abcd", 0, 5);
assertSubstring("a", "abcd", 0, 1);
assertSubstring("ab", "abcd", 0, 2);
assertSubstring("b", "abcd", 1, 1);
assertSubstring("d", "abcd", 3, 1);
assertSubstring("", "abcd", 4, 1);
assertSubstring("", "abcd", 3, 0);
assertSubstring("cd", "abcd", 2, -1);

// Passing a negative number for the start position should return an error.
assertException("abcd", -1, 4);
assertException("abcd", -1, 0);
assertException("abcd", -10, 0);

// See server6186.js for additional offset / length checks.

// Additional numeric types for offset / length.
assertSubstring("bc", "abcd", 1, 2);
assertSubstring("bc", "abcd", 1.0, 2.0);
assertSubstring("bc", "abcd", NumberInt("1"), NumberInt("2"));
assertSubstring("bc", "abcd", NumberLong("1"), NumberLong("2"));
assertSubstring("bc", "abcd", NumberInt("1"), NumberLong("2"));
assertSubstring("bc", "abcd", NumberLong("1"), NumberInt("2"));
assertSubstring("bc", "abcd", NumberDecimal("1"), NumberDecimal("2"));
// Integer component is used.
assertSubstring("bc", "abcd", 1.2, 2.2);
assertSubstring("bc", "abcd", 1.9, 2.9);
assertSubstring("cd", "abcd", 2, -1);
assertSubstring("abcd", "abcd", 0, -1);
// Any negative number for length will return the rest of the string.
assertSubstring("cd", "abcd", 2, -5);
assertSubstring("", "abcd", 4, -1);
assertSubstring("", "abcd", 10, -1);

// Non numeric types for offset / length.
assertException("abcd", false, 2);
assertException("abcd", 1, true);
assertException("abcd", "q", 2);
assertException("abcd", 1, "r");
assertException("abcd", null, 3);
assertException("abcd", 1, undefined);

// String coercion.
assertSubstring("123", 123, 0, 3);
assertSubstring("2", 123, 1, 1);
assertSubstring("1970", new Date(0), 0, 4);
assertSubstring("", null, 0, 4);
assertException(/abc/, 0, 4);

// Field path like string.
assertSubstring("$a", "a$a", 1, 2);

// Multi byte utf-8.
assertSubstring("\u0080", "\u0080", 0, 2);

assertException("\u0080", 0, 1);
assertException("\u0080", 1, 1);

assertSubstring("\u0080", "\u0080\u20ac", 0, 2);
assertSubstring("\u20ac", "\u0080\u20ac", 2, 3);

assertException("\u0080\u20ac", 1, 3);
assertException("\u0080\u20ac", 1, 4);
assertException("\u0080\u20ac", 0, 3);

assertException("\uD834\uDF06", 1, 4);
assertException("\uD834\uDF06", 0, 3);

assertSubstring("\u0044\u20ac", "\u0080\u0044\u20ac", 2, 4);
assertSubstring("\u0044", "\u0080\u0044\u20ac", 2, 1);

// The four byte utf-8 character 𝌆 (have to represent in surrogate halves).
assertSubstring("\uD834\uDF06", "\uD834\uDF06", 0, 4);

// Operands from document.
t.drop();
t.save({
    w: "ó",
    x: "a",
    y: "abc",
    z: "abcde",
    a: 0,
    b: 1,
    c: 2,
    d: 3,
    e: 4,
    f: 5,
    g: -2,
    /* Max unsigned int plus one */
    k: NumberLong(4294967297),
});
assertSubstring("a", "$x", "$a", "$b");
assertSubstring("abcde", "$z", "$a", "$k");
assertSubstring("", "$x", "$k", "$f");
assertSubstring("a", "$x", "$a", "$f");
assertSubstring("b", "$y", "$b", "$b");
assertSubstring("b", "$z", "$b", "$b");
assertSubstring("bcd", "$z", "$b", "$d");
assertSubstring("cde", "$z", "$c", "$f");
assertSubstring("c", "$y", "$c", "$f");
assertException("$w", "$b", "$d");
assertException("$w", "$a", "$c");
assertException("$w", "$a", "$g");
assertException("$w", "$g", "$a");

// String coercion fails.
assertErrorCode(t, [{$project: {a: {$substrCP: [new Map(), "$a", "$b"]}}}], 16007, "string coercion failed");

// Computed operands.
assertSubstring("cde", "$z", {$add: ["$b", "$b"]}, {$add: ["$c", "$d"]});
assertSubstring("cde", "$z", {$add: ["$b", 1]}, {$add: [2, "$d"]});

// Nested.
assert.eq(
    "e",
    t
        .aggregate({
            $project: {
                a: {
                    $substrBytes: [
                        {
                            $substrBytes: [{$substrBytes: [{$substrBytes: ["abcdefghij", 1, 6]}, 2, 5]}, 0, 3],
                        },
                        1,
                        1,
                    ],
                },
            },
        })
        .toArray()[0].a,
);
