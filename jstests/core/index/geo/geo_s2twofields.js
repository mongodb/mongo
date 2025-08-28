// Verify that we can index multiple geo fields with 2dsphere, and that
// performance is what we expect it to be with indexing both fields.
//
// @tags: [
//   operations_longer_than_stepdown_interval_in_txns,
//   requires_fastcount,
// ]

let t = db.geo_s2twofields;
t.drop();

Random.setRandomSeed();
let random = Random.rand;
let PI = Math.PI;

function randomCoord(center, minDistDeg, maxDistDeg) {
    let dx = random() * (maxDistDeg - minDistDeg) + minDistDeg;
    let dy = random() * (maxDistDeg - minDistDeg) + minDistDeg;
    return [center[0] + dx, center[1] + dy];
}

let nyc = {type: "Point", coordinates: [-74.0064, 40.7142]};
let miami = {type: "Point", coordinates: [-80.1303, 25.7903]};
let maxPoints = 10000;
let degrees = 5;

let arr = [];
for (let i = 0; i < maxPoints; ++i) {
    let fromCoord = randomCoord(nyc.coordinates, 0, degrees);
    let toCoord = randomCoord(miami.coordinates, 0, degrees);

    arr.push({from: {type: "Point", coordinates: fromCoord}, to: {type: "Point", coordinates: toCoord}});
}
let res = t.insert(arr);
assert.commandWorked(res);
assert.eq(t.count(), maxPoints);

function semiRigorousTime(func) {
    let lowestTime = func();
    let iter = 2;
    for (let i = 0; i < iter; ++i) {
        let run = func();
        if (run < lowestTime) {
            lowestTime = run;
        }
    }
    return lowestTime;
}

function timeWithoutAndWithAnIndex(index, query) {
    t.dropIndex(index);
    let withoutTime = semiRigorousTime(function () {
        return t.find(query).explain("executionStats").executionStats.executionTimeMillis;
    });
    t.createIndex(index);
    let withTime = semiRigorousTime(function () {
        return t.find(query).explain("executionStats").executionStats.executionTimeMillis;
    });
    t.dropIndex(index);
    return [withoutTime, withTime];
}

let maxQueryRad = (0.5 * PI) / 180.0;
// When we're not looking at ALL the data, anything indexed should beat not-indexed.
var smallQuery = timeWithoutAndWithAnIndex(
    {to: "2dsphere", from: "2dsphere"},
    {
        from: {$within: {$centerSphere: [nyc.coordinates, maxQueryRad]}},
        to: {$within: {$centerSphere: [miami.coordinates, maxQueryRad]}},
    },
);
print("Indexed time " + smallQuery[1] + " unindexed " + smallQuery[0]);
// assert(smallQuery[0] > smallQuery[1]);

// Let's just index one field.
var smallQuery = timeWithoutAndWithAnIndex(
    {to: "2dsphere"},
    {
        from: {$within: {$centerSphere: [nyc.coordinates, maxQueryRad]}},
        to: {$within: {$centerSphere: [miami.coordinates, maxQueryRad]}},
    },
);
print("Indexed time " + smallQuery[1] + " unindexed " + smallQuery[0]);
// assert(smallQuery[0] > smallQuery[1]);

// And the other one.
var smallQuery = timeWithoutAndWithAnIndex(
    {from: "2dsphere"},
    {
        from: {$within: {$centerSphere: [nyc.coordinates, maxQueryRad]}},
        to: {$within: {$centerSphere: [miami.coordinates, maxQueryRad]}},
    },
);
print("Indexed time " + smallQuery[1] + " unindexed " + smallQuery[0]);
// assert(smallQuery[0] > smallQuery[1]);
