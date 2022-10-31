(function() {
"use strict";

const coll = db.cqf_elemMatch;
coll.drop();

jsTestLog('Inserting docs:');

const docs = [
    {a: [1, 2, 3, 4, 5, 6]},
    {a: [5, 6, 7, 8, 9]},
    {a: [1, 2, 3]},
    {a: []},
    {a: [1]},
    {a: [10]},
    {a: 5},
    {a: 6},
    {a: [[6]]},
    {a: [[[6]]]},
    {a: [{b: [6]}]},
    {a: [[{b: [6]}]]},
];
show(docs);

assert.commandWorked(coll.insert(docs));

function runPipeline(pipeline) {
    pipeline.push({$project: {_id: 0}});
    jsTestLog(`Pipeline: ${tojsononeline(pipeline)}`);
    show(coll.aggregate(pipeline));
}

// Value elemMatch.
let pipeline = [{$match: {a: {$elemMatch: {$gte: 5, $lte: 6}}}}];
runPipeline(pipeline);

pipeline = [{$match: {a: {$elemMatch: {$lt: 11, $gt: 9}}}}];
runPipeline(pipeline);

// Contradiction.
pipeline = [{$match: {a: {$elemMatch: {$lt: 5, $gt: 6}}}}];
runPipeline(pipeline);

// Nested elemMatch.
pipeline = [{$match: {a: {$elemMatch: {$elemMatch: {$gte: 5, $lte: 6}}}}}];
runPipeline(pipeline);

pipeline = [{$match: {a: {$elemMatch: {$elemMatch: {$elemMatch: {$gte: 5, $lte: 6}}}}}}];
runPipeline(pipeline);

// Various expressions under $elemMatch.
pipeline = [{$match: {a: {$elemMatch: {$size: 1}}}}];
runPipeline(pipeline);

pipeline = [{$match: {a: {$elemMatch: {$exists: true}}}}];
runPipeline(pipeline);

// Test for a value $elemMatch nested under an object $elemMatch.
pipeline = [{$match: {a: {$elemMatch: {b: {$elemMatch: {$gt: 5}}}}}}];
runPipeline(pipeline);

pipeline = [{$match: {a: {$elemMatch: {$elemMatch: {b: {$elemMatch: {$gt: 5}}}}}}}];
runPipeline(pipeline);
}());
