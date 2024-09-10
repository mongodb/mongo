// In SERVER-22580 and SERVER-51557, the $substrCP expression was introduced. In this file, we test
// the error cases and the intended behavior of this expression.
import "jstests/libs/sbe_assert_error_override.js";
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

var coll = db.substrCP;
coll.drop();

// Need an empty document for pipeline.
assert.commandWorked(coll.insertOne({"a": "abc"}));

function assertArgsException(errorCode, args) {
    assertErrorCode(
        coll, [{$project: {a: {$substrCP: args}}}], errorCode, "error thrown, check parameters");
}

function assertSubstring(expected, str, offset, len) {
    assert.eq(expected,
              coll.aggregate({$project: {a: {$substrCP: [str, offset, len]}}}).toArray()[0].a);
}

function assertSubstringBytes(expected, str, offset, len) {
    assert.eq(expected,
              coll.aggregate({$project: {a: {$substrBytes: [str, offset, len]}}}).toArray()[0].a);
}

assertArgsException(34452, ["$a", 0, "a"]);
assertArgsException(34453, ["$a", 0, NaN]);
assertArgsException(34450, ["$a", "abc", 3]);
assertArgsException(34451, ["$a", 2.2, 3]);
assertArgsException(34455, ["$a", -1, 3]);
assertArgsException(34454, ["$a", 1, -3]);

assert(coll.drop());
coll.insertOne({
    t: "寿司sushi",
    u: "éclair",
    v: "Å",
    w: "◢◢◢",
    x: 'a',
    y: 'abc',
    z: 'abcde',
    a: 0,
    b: 1,
    c: 2,
    d: 3,
    e: 4,
    f: 5,
    neg: -4,
    invalidStr: new Map(),
    /* Max unsigned int plus one */
    bigNum: NumberLong(4294967297)
});

// Wrong number of args.
assertArgsException(16020, []);
assertArgsException(16020, ['$x']);
assertArgsException(16020, ['$x', 1, 2, 3]);

// Tests that $substrCP performs as expected.
assertSubstring("", "$x", 999, 0);
assertSubstring("", "$x", 999, 1);
assertSubstring("Å", "$v", "$a", "$b");
assertSubstring("a", "$x", "$a", "$b");
assertSubstring("◢◢", "$w", "$b", "$c");
assertSubstring("cde", "$z", "$c", "$d");

// Covers additional errors and ensures errors are thrown following document retrival.
assertArgsException(5155700, ['a', '$invalidStr', 7]);
assertArgsException(5155700, ['a', '$bigNum', 7]);
assertArgsException(5155702, ['a', 0, "$t"]);
assertArgsException(5155701, ['a', "$neg", 2]);
assertArgsException(5155703, ['a', 0, "$neg"]);

// String coercion fails as expected.
assertArgsException(16007, ['$invalidStr', '$a', '$b']);

// Computed operands.
assertSubstring('cde', '$z', {$add: ['$b', '$b']}, {$add: ['$c', '$d']});
assertSubstring('cde', '$z', {$add: ['$b', 1]}, {$add: [2, '$d']});

// Nested.
assert.eq(
    'e',
    coll.aggregate({
            $project: {
                a: {
                    $substrCP: [
                        {
                            $substrCP:
                                [{$substrCP: [{$substrCP: ['abcdefghij', '$b', 6]}, '$c', 5]}, 0, 3]
                        },
                        1,
                        1
                    ]
                }
            }
        })
        .toArray()[0]
        .a);

// $substrBytes and $substrCP returns different results when encountering multi-byte
// utf-chars.
assertSubstring("écla", "$u", "$a", "$e");
assertSubstringBytes("écl", "$u", "$a", "$e");
assertSubstring("寿司sush", "$t", "$a", 6);
assertSubstringBytes("寿司", "$t", "$a", 6);
