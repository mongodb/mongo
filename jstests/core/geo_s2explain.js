// Test to check whether the number of intervals in a geoNear query equals
// the number of inputStages it completes

var t = db.jstests_geo_s2explain;
t.drop();

var point1 = {loc: {type: "Point", coordinates: [10, 10]}};
var point2 = {loc: {type: "Point", coordinates: [10.001, 10]}};
assert.writeOK(t.insert([point1, point2]));

assert.commandWorked(t.ensureIndex({loc: "2dsphere"}));

var explain = t.find({loc: {$nearSphere: {type: "Point", coordinates: [10, 10]}}})
                  .limit(1)
                  .explain("executionStats");
var inputStage = explain.executionStats.executionStages.inputStage;

assert.eq(1, inputStage.searchIntervals.length);

// Populates the collection with a few hundred points at varying distances
var points = [];
for (var i = 10; i < 70; i += 0.1) {
    points.push({loc: {type: "Point", coordinates: [i, i]}});
}

assert.writeOK(t.insert(points));

explain = t.find({loc: {$nearSphere: {type: "Point", coordinates: [10, 10]}}})
              .limit(10)
              .explain("executionStats");
inputStage = explain.executionStats.executionStages.inputStage;

assert.eq(inputStage.inputStages.length, inputStage.searchIntervals.length);

explain = t.find({loc: {$nearSphere: {type: "Point", coordinates: [10, 10]}}})
              .limit(50)
              .explain("executionStats");
inputStage = explain.executionStats.executionStages.inputStage;

assert.eq(inputStage.inputStages.length, inputStage.searchIntervals.length);

explain = t.find({loc: {$nearSphere: {type: "Point", coordinates: [10, 10]}}})
              .limit(200)
              .explain("executionStats");
inputStage = explain.executionStats.executionStages.inputStage;

assert.eq(inputStage.inputStages.length, inputStage.searchIntervals.length);
