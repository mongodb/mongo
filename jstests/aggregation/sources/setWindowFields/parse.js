/**
 * Test the user-facing syntax of $setWindowFields. For example:
 * - Which options are allowed?
 * - When is an expression expected, vs a constant?
 * - Which window functions accept bounds?
 * - When something is not allowed, what error message do we expect?
 */
(function() {
"use strict";

const coll = db.setWindowFields_parse;
coll.drop();

function run(stage, extraCommandArgs = {}) {
    return coll.runCommand(
        Object.merge({aggregate: coll.getName(), pipeline: [stage], cursor: {}}, extraCommandArgs));
}

// Test that the stage spec must be an object.
assert.commandFailedWithCode(run({$setWindowFields: "invalid"}), ErrorCodes.FailedToParse);

// Test that the stage parameters are the correct type.
assert.commandFailedWithCode(run({$setWindowFields: {sortBy: "invalid"}}), ErrorCodes.TypeMismatch);
assert.commandFailedWithCode(run({$setWindowFields: {output: "invalid"}}), ErrorCodes.TypeMismatch);

// Test that parsing fails for an invalid partitionBy expression.
assert.commandFailedWithCode(
    run({$setWindowFields: {partitionBy: {$notAnOperator: 1}, output: {}}}),
    ErrorCodes.InvalidPipelineOperator);

// Since partitionBy can be any expression, it can be a variable.
assert.commandWorked(run({$setWindowFields: {partitionBy: "$$NOW", output: {}}}));
assert.commandWorked(
    run({$setWindowFields: {partitionBy: "$$myobj.a", output: {}}}, {let : {myobj: {a: 456}}}));

// Test that parsing fails for unrecognized parameters.
assert.commandFailedWithCode(run({$setWindowFields: {what_is_this: 1}}), 40415);

// Test for a successful parse, ignoring the response documents.
assert.commandWorked(run({
    $setWindowFields: {
        partitionBy: "$state",
        sortBy: {city: 1},
        output: {a: {$sum: 1, window: {documents: ["unbounded", "current"]}}}
    }
}));

function runWindowFunction(spec) {
    // Include a single-field sortBy in this helper to allow all kinds of bounds.
    return run({$setWindowFields: {sortBy: {ts: 1}, output: {v: spec}}});
}

// The most basic case: $sum everything.
assert.commandWorked(runWindowFunction({$sum: "$a"}));

// That's equivalent to bounds of [unbounded, unbounded].
assert.commandWorked(
    runWindowFunction({$sum: "$a", window: {documents: ['unbounded', 'unbounded']}}));

// Extra arguments to a window function are rejected.
assert.commandFailedWithCode(runWindowFunction({abcde: 1}),
                             ErrorCodes.FailedToParse,
                             'Window function $sum found an unknown argument: abcde');

// Bounds can be bounded, or bounded on one side.
assert.commandWorked(runWindowFunction({"$sum": "$a", window: {documents: [-2, +4]}}));
assert.commandWorked(runWindowFunction({"$sum": "$a", window: {documents: [-3, 'unbounded']}}));
assert.commandWorked(runWindowFunction({"$sum": "$a", window: {documents: ['unbounded', +5]}}));
assert.commandWorked(runWindowFunction({"$max": "$a", window: {documents: [-3, 'unbounded']}}));

// Range-based bounds:
assert.commandWorked(runWindowFunction({$sum: "$a", window: {range: ['unbounded', 'unbounded']}}));
assert.commandWorked(runWindowFunction({$sum: "$a", window: {range: [-2, +4]}}));
assert.commandWorked(runWindowFunction({$sum: "$a", window: {range: [-3, 'unbounded']}}));
assert.commandWorked(runWindowFunction({$sum: "$a", window: {range: ['unbounded', +5]}}));
assert.commandWorked(
    runWindowFunction({$sum: "$a", window: {range: [NumberDecimal('1.42'), NumberLong(5)]}}));

// Time-based bounds:
assert.commandWorked(
    runWindowFunction({"$sum": "$a", window: {range: [-3, 'unbounded'], unit: 'hour'}}));

// Numeric bounds can be a constant expression:
let expr = {$add: [2, 2]};
assert.commandWorked(runWindowFunction({"$sum": "$a", window: {documents: [expr, expr]}}));
assert.commandWorked(runWindowFunction({"$sum": "$a", window: {range: [expr, expr]}}));
assert.commandWorked(
    runWindowFunction({"$sum": "$a", window: {range: [expr, expr], unit: 'hour'}}));
// But 'current' and 'unbounded' are not expressions: they're more like keywords.
assert.commandFailedWithCode(
    runWindowFunction({"$sum": "$a", window: {documents: [{$const: 'current'}, 3]}}),
    ErrorCodes.FailedToParse,
    'Numeric document-based bounds must be an integer');
assert.commandFailedWithCode(runWindowFunction({"$sum": "$a", range: [{$const: 'current'}, 3]}),
                             ErrorCodes.FailedToParse,
                             'Range-based bounds expression must be a number');

// Bounds must not be backwards.
function badBounds(bounds) {
    assert.commandFailedWithCode(runWindowFunction(Object.merge({"$sum": "$a"}, {window: bounds})),
                                 5339900,
                                 'Lower bound must not exceed upper bound');
}
badBounds({documents: [+1, -1]});
badBounds({range: [+1, -1]});
badBounds({range: [+1, -1], unit: 'day'});

badBounds({documents: ['current', -1]});
badBounds({range: ['current', -1]});
badBounds({range: ['current', -1], unit: 'day'});

badBounds({documents: [+1, 'current']});
badBounds({range: [+1, 'current']});
badBounds({range: [+1, 'current'], unit: 'day'});

// Any bound besides [unbounded, unbounded] requires a sort:
// - document-based
assert.commandWorked(run({
    $setWindowFields:
        {output: {v: {$sum: "$a", window: {documents: ['unbounded', 'unbounded']}}}}
}));
assert.commandFailedWithCode(
    run({
        $setWindowFields:
            {output: {v: {$sum: "$a", window: {documents: ['unbounded', 'current']}}}}
    }),
    5339901,
    'Document-based bounds require a sortBy');
// - range-based
assert.commandFailedWithCode(
    run({
        $setWindowFields: {output: {v: {$sum: "$a", window: {range: ['unbounded', 'unbounded']}}}}
    }),
    5339902,
    'Range-based bounds require sortBy a single field');
assert.commandFailedWithCode(
    run({
        $setWindowFields: {
            sortBy: {a: 1, b: 1},
            output: {v: {$sum: "$a", window: {range: ['unbounded', 'unbounded']}}}
        }
    }),
    5339902,
    'Range-based bounds require sortBy a single field');
assert.commandFailedWithCode(
    run({$setWindowFields: {output: {v: {$sum: "$a", window: {range: ['unbounded', 'current']}}}}}),
    5339902,
    'Range-based bounds require sortBy a single field');
assert.commandFailedWithCode(
    run({
        $setWindowFields: {
            sortBy: {a: 1, b: 1},
            output: {v: {$sum: "$a", window: {range: ['unbounded', 'current']}}}
        }
    }),
    5339902,
    'Range-based bounds require sortBy a single field');
// - time-based
assert.commandFailedWithCode(
    run({
        $setWindowFields:
            {output: {v: {$sum: "$a", window: {range: ['unbounded', 'unbounded'], unit: 'second'}}}}
    }),
    5339902);
assert.commandFailedWithCode(
    run({
        $setWindowFields: {
            sortBy: {a: 1, b: 1},
            output: {v: {$sum: "$a", window: {range: ['unbounded', 'unbounded'], unit: 'second'}}}
        }
    }),
    5339902);
assert.commandFailedWithCode(
    run({
        $setWindowFields:
            {output: {v: {$sum: "$a", window: {range: ['unbounded', 'current'], unit: 'second'}}}}
    }),
    5339902,
    'Range-based bounds require sortBy a single field');
assert.commandFailedWithCode(
    run({
        $setWindowFields: {
            sortBy: {a: 1, b: 1},
            output: {v: {$sum: "$a", window: {range: ['unbounded', 'current'], unit: 'second'}}}
        }
    }),
    5339902,
    'Range-based bounds require sortBy a single field');

// Variety of accumulators:
assert.commandWorked(run({
    $setWindowFields:
        {sortBy: {ts: 1},
         output: {v: {$sum: "$a", window: {documents: ['unbounded', 'current']}}}}
}));
assert.commandWorked(run({
    $setWindowFields:
        {sortBy: {ts: 1},
         output: {v: {$avg: "$a", window: {documents: ['unbounded', 'current']}}}}
}));
assert.commandWorked(run({
    $setWindowFields:
        {sortBy: {ts: 1},
         output: {v: {$max: "$a", window: {documents: ['unbounded', 'current']}}}}
}));
assert.commandWorked(run({
    $setWindowFields:
        {sortBy: {ts: 1},
         output: {v: {$min: "$a", window: {documents: ['unbounded', 'current']}}}}
}));

// Not every accumulator is automatically a window function.
assert.commandFailedWithCode(run({$setWindowFields: {output: {v: {$mergeObjects: "$a"}}}}),
                             ErrorCodes.FailedToParse,
                             'No such window function: $mergeObjects');
assert.commandFailedWithCode(run({$setWindowFields: {output: {v: {$accumulator: "$a"}}}}),
                             ErrorCodes.FailedToParse,
                             'No such window function: $accumulator');

// Test that an empty object is a valid projected field.
assert.commandWorked(coll.insert({}));
assert.commandWorked(run({$setWindowFields: {output: {v: {$max: {mergeObjects: {}}}}}}));

// However conflicting field paths is always an error.
const err = assert.commandFailedWithCode(
    run({$setWindowFields: {output: {a: {$sum: 1}, 'a.b': {$sum: 1}}}}), 6307900);
assert.includes(err.errmsg, 'specification contains two conflicting paths');
})();
