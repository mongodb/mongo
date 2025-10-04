// $cond returns the evaluated second argument if the first evaluates to true but the evaluated
// third argument if the first evaluates to false.
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.jstests_aggregation_cond;
coll.drop();

coll.save({});

function assertError(expectedErrorCode, condSpec) {
    assertErrorCode(coll, {$project: {a: {$cond: condSpec}}}, expectedErrorCode);
}

function assertResult(expectedResult, arg) {
    assert.eq(expectedResult, coll.aggregate({$project: {a: {$cond: arg}}}).toArray()[0].a);
}

// Wrong number of args.
assertError(16020, []);
assertError(16020, [1]);
assertError(16020, [false]);
assertError(16020, [1, 1]);
assertError(16020, [1, 1, null, 1]);
assertError(16020, [1, 1, 1, undefined]);

// Bad object cases.
assertError(17080, {"else": 1, then: 1});
assertError(17081, {"if": 1, "else": 1});
assertError(17082, {"if": 1, then: 1});
assertError(17083, {asdf: 1, then: 1});

// Literal expressions.
assertResult(1, [true, 1, 2]);
assertResult(2, [false, 1, 2]);

// Order independence for object case.
assertResult(1, {"if": true, "then": 1, "else": 2});
assertResult(1, {"if": true, "else": 2, "then": 1});
assertResult(1, {"then": 1, "if": true, "else": 2});
assertResult(1, {"then": 1, "else": 2, "if": true});
assertResult(1, {"else": 2, "then": 1, "if": true});
assertResult(1, {"else": 2, "if": true, "then": 1});

// Computed expressions.
assertResult(1, [{$and: []}, {$add: [1]}, {$add: [1, 1]}]);
assertResult(2, [{$or: []}, {$add: [1]}, {$add: [1, 1]}]);

assert(coll.drop());
assert.commandWorked(coll.insert({t: true, f: false, x: "foo", y: "bar"}));

// Field path expressions.
assertResult("foo", ["$t", "$x", "$y"]);
assertResult("bar", ["$f", "$x", "$y"]);

assert(coll.drop());
assert.commandWorked(coll.insert({}));

// Coerce to bool.
assertResult("a", [1, "a", "b"]);
assertResult("a", ["", "a", "b"]);
assertResult("b", [0, "a", "b"]);

// Nested.
assert(coll.drop());
assert.commandWorked(coll.insert({noonSense: "am", mealCombined: "no"}));
assert.commandWorked(coll.insert({noonSense: "am", mealCombined: "yes"}));
assert.commandWorked(coll.insert({noonSense: "pm", mealCombined: "yes"}));
assert.commandWorked(coll.insert({noonSense: "pm", mealCombined: "no"}));
assert.eq(
    ["breakfast", "brunch", "dinner", "linner"],
    coll
        .aggregate([
            {
                $project: {
                    meal: {
                        $cond: [
                            {$eq: ["$noonSense", "am"]},
                            {$cond: [{$eq: ["$mealCombined", "yes"]}, "brunch", "breakfast"]},
                            {$cond: [{$eq: ["$mealCombined", "yes"]}, "linner", "dinner"]},
                        ],
                    },
                },
            },
            {$sort: {meal: 1}},
        ])
        .map((doc) => doc.meal),
);
