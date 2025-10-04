// Test to check whether the number of intervals in a geoNear query equals
// the number of inputStages it completes

import {getExecutionStages} from "jstests/libs/query/analyze_plan.js";

let t = db.jstests_geo_s2explain;
t.drop();

let point1 = {loc: {type: "Point", coordinates: [10, 10]}};
let point2 = {loc: {type: "Point", coordinates: [10.001, 10]}};
assert.commandWorked(t.insert([point1, point2]));

assert.commandWorked(t.createIndex({loc: "2dsphere"}));

let explain = t
    .find({loc: {$nearSphere: {type: "Point", coordinates: [10, 10]}}})
    .limit(1)
    .explain("executionStats");
let inputStage = getExecutionStages(explain)[0].inputStage;

assert.eq(1, inputStage.searchIntervals.length);

// Populates the collection with a few hundred points at varying distances
let points = [];
for (let i = 10; i < 70; i += 0.1) {
    points.push({loc: {type: "Point", coordinates: [i, i]}});
}

assert.commandWorked(t.insert(points));

explain = t
    .find({loc: {$nearSphere: {type: "Point", coordinates: [10, 10]}}})
    .limit(10)
    .explain("executionStats");
inputStage = getExecutionStages(explain)[0].inputStage;

assert.eq(inputStage.inputStages.length, inputStage.searchIntervals.length);

explain = t
    .find({loc: {$nearSphere: {type: "Point", coordinates: [10, 10]}}})
    .limit(50)
    .explain("executionStats");
inputStage = getExecutionStages(explain)[0].inputStage;

assert.eq(inputStage.inputStages.length, inputStage.searchIntervals.length);

explain = t
    .find({loc: {$nearSphere: {type: "Point", coordinates: [10, 10]}}})
    .limit(200)
    .explain("executionStats");
inputStage = getExecutionStages(explain)[0].inputStage;

assert.eq(inputStage.inputStages.length, inputStage.searchIntervals.length);
