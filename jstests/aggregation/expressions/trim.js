/**
 * Basic tests for the $trim, $ltrim, and $rtrim expressions.
 */
import "jstests/libs/query/sbe_assert_error_override.js";

import {
    assertErrorCode,
    testExpression,
    testExpressionWithCollation
} from "jstests/aggregation/extras/utils.js";

const coll = db.trim_expressions;

testExpression(
    coll,
    {$map: {input: {$split: ["4, 5, 6, 7,8,9, 10", ","]}, in : {$trim: {input: "$$this"}}}},
    ["4", "5", "6", "7", "8", "9", "10"]);

// Test that the trim expressions do not respect the collation.
const caseInsensitive = {
    locale: "en_US",
    strength: 2
};

assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 0, trimTest: " abc "}, {_id: 1, trimTest: " a b\nc "}]));
assert.eq(
    coll.aggregate([{$project: {exp: {$trim: {input: "$trimTest"}}}}, {$sort: {_id: 1}}]).toArray(),
    [{_id: 0, exp: "abc"}, {_id: 1, exp: "a b\nc"}]);

assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 2, ltrimTest: "\t abc "}]));
assert.eq(coll.aggregate([{$project: {exp: {$ltrim: {input: "$ltrimTest"}}}}]).toArray(),
          [{_id: 2, exp: "abc "}]);

assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 3, rtrimTest: "\t abc "}]));
assert.eq(coll.aggregate([{$project: {exp: {$rtrim: {input: "$rtrimTest"}}}}]).toArray(),
          [{_id: 3, exp: "\t abc"}]);

assert(coll.drop());
assert.commandWorked(coll.insert([{_id: 4, caseTest: "xXx", chars: "x"}]));

const caseTestMap = {
    "$trim": "X",
    "$ltrim": "Xx",
    "$rtrim": "xX"
};
for (const op of ["$trim", "$ltrim", "$rtrim"]) {
    const expected = caseTestMap[op];
    assert.eq(coll.aggregate([{$project: {exp: {[op]: {input: "$caseTest", chars: "$chars"}}}}],
                             {collation: caseInsensitive})
                  .toArray(),
              [{_id: 4, exp: expected}]);
}

// Test using inputs from documents.
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, name: ", Charlie"},
    {_id: 1, name: "Obama\t,  Barack"},
    {_id: 2, name: " Ride,  Sally   "}
]));

assert.eq(
    coll.aggregate([
            {
                $project:
                    {firstName: {$trim: {input: {$arrayElemAt: [{$split: ["$name", ","]}, 1]}}}}
            },
            {$sort: {_id: 1}}
        ])
        .toArray(),
    [{_id: 0, firstName: "Charlie"}, {_id: 1, firstName: "Barack"}, {_id: 2, firstName: "Sally"}]);

assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, poorlyParsedWebTitle: "The title of my document"},
    {_id: 1, poorlyParsedWebTitle: "\u2001\u2002 Odd unicode indentation"},
    {_id: 2, poorlyParsedWebTitle: "\u2001\u2002 Odd unicode indentation\u200A"},
]));
assert.eq(
    coll.aggregate(
            [{$project: {title: {$ltrim: {input: "$poorlyParsedWebTitle"}}}}, {$sort: {_id: 1}}])
        .toArray(),
    [
        {_id: 0, title: "The title of my document"},
        {_id: 1, title: "Odd unicode indentation"},
        {_id: 2, title: "Odd unicode indentation\u200A"}
    ]);

assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, proof: "Left as an exercise for the reader∎"},
    {_id: 1, proof: "∎∃ proof∎"},
    {_id: 2, proof: "Just view the problem as a continuous DAG whose elements are taylor series∎"},
    {_id: 3, proof: null},
    {_id: 4},
]));
assert.eq(
    coll.aggregate(
            [{$project: {proof: {$rtrim: {input: "$proof", chars: "∎"}}}}, {$sort: {_id: 1}}])
        .toArray(),
    [
        {_id: 0, proof: "Left as an exercise for the reader"},
        {_id: 1, proof: "∎∃ proof"},
        {
            _id: 2,
            proof: "Just view the problem as a continuous DAG whose elements are taylor series"
        },
        {_id: 3, proof: null},
        {_id: 4, proof: null},
    ]);

// Semantically same as the tests above but non-constant input for 'chars'
assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, proof: "Left as an exercise for the reader∎", extra: "∎"},
    {_id: 1, proof: "∎∃ proof∎", extra: "∎"},
    {
        _id: 2,
        proof: "Just view the problem as a continuous DAG whose elements are taylor series∎",
        extra: "∎"
    },
    {_id: 3, proof: null},
    {_id: 4},
]));
assert.eq(
    coll.aggregate(
            [{$project: {proof: {$rtrim: {input: "$proof", chars: "$extra"}}}}, {$sort: {_id: 1}}])
        .toArray(),
    [
        {_id: 0, proof: "Left as an exercise for the reader"},
        {_id: 1, proof: "∎∃ proof"},
        {
            _id: 2,
            proof: "Just view the problem as a continuous DAG whose elements are taylor series"
        },
        {_id: 3, proof: null},
        {_id: 4, proof: null},
    ]);

assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, proof: "Left as an exercise for the reader∎", extra: null},
    {_id: 1, proof: "∎∃ proof∎", extra: null}
]));
assert.eq(
    coll.aggregate(
            [{$project: {proof: {$rtrim: {input: "$proof", chars: "$extra"}}}}, {$sort: {_id: 1}}])
        .toArray(),
    [{_id: 0, proof: null}, {_id: 1, proof: null}]);

assert(coll.drop());
assert.commandWorked(coll.insert([
    {_id: 0, nonObject: " x "},
    {_id: 1, constantNum: 4},
]));

// Test that errors are reported correctly (for all of $trim, $ltrim, $rtrim).
for (const op of ["$trim", "$ltrim", "$rtrim"]) {
    assertErrorCode(coll, [{$project: {x: {[op]: {}}}}], 50695);
    assertErrorCode(coll, [{$project: {x: {[op]: "$nonObject"}}}], 50696);
    assertErrorCode(coll, [{$project: {x: {[op]: {input: "$constantNum"}}}}], 50699);
    assertErrorCode(
        coll, [{$project: {x: {[op]: {input: {$add: ["$constantNum", "$constantNum"]}}}}}], 50699);
    assertErrorCode(coll, [{$project: {x: {[op]: {input: "$_id"}}}}], 50699);
    assertErrorCode(coll, [{$project: {x: {[op]: {input: "$nonObject", chars: "$_id"}}}}], 50700);
}
