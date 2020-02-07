// Tests basic functionality of the $_internalJs expression.
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

const coll = db.internal_js;
coll.drop();

function f_finalize(first, second) {
    return first + second;
}

for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({value: i}));
}

let pipeline = [{
    $project: {
        newValue: {
            $_internalJs: {
                args: ["$value", -1],
                eval: f_finalize,
            },
        },
        _id: 0,
    }
}];

let results = coll.aggregate(pipeline, {cursor: {batchSize: 1}}).toArray();
assert(resultsEq(results,
                 [{newValue: -1}, {newValue: 0}, {newValue: 1}, {newValue: 2}, {newValue: 3}]),
       results);

// Test that the 'eval' function accepts a string argument.
pipeline[0].$project.newValue.$_internalJs.eval = f_finalize.toString();
results = coll.aggregate(pipeline, {cursor: {}}).toArray();
assert(resultsEq(results,
                 [{newValue: -1}, {newValue: 0}, {newValue: 1}, {newValue: 2}, {newValue: 3}]),
       results);

// Test that internalJs can take an expression that evaluates to an array for the 'args' parameter.
coll.drop();
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({values: [i, i * 2]}));
}
pipeline = [{
    $project: {
        newValue: {
            $_internalJs: {
                args: "$values",
                eval: f_finalize,
            },
        },
        _id: 0,
    }
}];

results = coll.aggregate(pipeline, {cursor: {}}).toArray();
assert(resultsEq(results,
                 [{newValue: 0}, {newValue: 3}, {newValue: 6}, {newValue: 9}, {newValue: 12}]),
       results);

// Test that the command correctly fails for invalid arguments.
pipeline = [{
    $project: {
        newValue: {
            $_internalJs: {
                'args': 'must evaluate to an array',
                'eval': f_finalize,
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), 31266);

pipeline = [{
    $project: {
        newValue: {
            $_internalJs: {
                'args': [1, 3],
                'eval': 'this is not a valid function!',
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}),
    ErrorCodes.JSInterpreterFailure);

// Test that we fail if the 'args' field is not an array.
pipeline = [{
    $project: {
        newValue: {
            $_internalJs: {
                'args': "A string!",
                'eval': f_finalize,
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), 31266);
})();
