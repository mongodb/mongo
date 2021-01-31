// Tests basic functionality of the $function expression.
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');

const coll = db.expression_function;
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
            $function: {
                args: ["$value", -1],
                body: f_finalize,
                lang: "js",
            },
        },
        _id: 0,
    }
}];

let results = coll.aggregate(pipeline, {cursor: {batchSize: 1}}).toArray();
assert(resultsEq(results,
                 [{newValue: -1}, {newValue: 0}, {newValue: 1}, {newValue: 2}, {newValue: 3}]),
       results);

// Test that the 'body' function accepts a string argument.
pipeline[0].$project.newValue.$function.body = f_finalize.toString();
results = coll.aggregate(pipeline, {cursor: {}}).toArray();
assert(resultsEq(results,
                 [{newValue: -1}, {newValue: 0}, {newValue: 1}, {newValue: 2}, {newValue: 3}]),
       results);

// Test that function can take an expression that evaluates to an array for the 'args' parameter.
coll.drop();
for (let i = 0; i < 5; i++) {
    assert.commandWorked(coll.insert({values: [i, i * 2]}));
}
pipeline = [{
    $project: {
        newValue: {
            $function: {
                args: "$values",
                body: f_finalize,
                lang: "js",
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
            $function: {
                'args': 'must evaluate to an array',
                'body': f_finalize,
                'lang': 'js',
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
            $function: {
                'args': [1, 3],
                'body': 'this is not a valid function!',
                'lang': 'js',
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}),
    ErrorCodes.JSInterpreterFailure);

pipeline = [{
    $project: {
        newValue: {
            $function: {
                'args': [1, 3],
                'body': f_finalize,
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), 31418);

pipeline = [{
    $project: {
        newValue: {
            $function: {
                'args': [1, 3],
                'body': f_finalize,
                'lang': 'not js!',
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), 31419);

// Test that we fail if the 'args' field is not an array.
pipeline = [{
    $project: {
        newValue: {
            $function: {
                'args': "A string!",
                'body': f_finalize,
                'lang': 'js',
            },
        },
        _id: 0,
    }
}];
assert.commandFailedWithCode(
    db.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}}), 31266);
})();
