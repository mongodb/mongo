// Verify that we can index multiple geo fields with 2dsphere, and that
// performance is what we expect it to be with indexing both fields.
var t = db.geo_s2twofields;
t.drop();

Random.setRandomSeed();
var random = Random.rand;
var PI = Math.PI;

function randomCoord(center, minDistDeg, maxDistDeg) {
    var dx = random() * (maxDistDeg - minDistDeg) + minDistDeg;
    var dy = random() * (maxDistDeg - minDistDeg) + minDistDeg;
    return [center[0] + dx, center[1] + dy];
}

var nyc = {type: "Point", coordinates: [-74.0064, 40.7142]};
var miami = {type: "Point", coordinates: [-80.1303, 25.7903]};
var maxPoints = 10000;
var degrees = 5;

var arr = [];
for (var i = 0; i < maxPoints; ++i) {
    var fromCoord = randomCoord(nyc.coordinates, 0, degrees);
    var toCoord = randomCoord(miami.coordinates, 0, degrees);

    arr.push(
        {from: {type: "Point", coordinates: fromCoord}, to: {type: "Point", coordinates: toCoord}});
}
res = t.insert(arr);
assert.writeOK(res);
assert.eq(t.count(), maxPoints);

function semiRigorousTime(func) {
    var lowestTime = func();
    var iter = 2;
    for (var i = 0; i < iter; ++i) {
        var run = func();
        if (run < lowestTime) {
            lowestTime = run;
        }
    }
    return lowestTime;
}

function timeWithoutAndWithAnIndex(index, query) {
    t.dropIndex(index);
    var withoutTime = semiRigorousTime(function() {
        return t.find(query).explain("executionStats").executionStats.executionTimeMillis;
    });
    t.ensureIndex(index);
    var withTime = semiRigorousTime(function() {
        return t.find(query).explain("executionStats").executionStats.executionTimeMillis;
    });
    t.dropIndex(index);
    return [withoutTime, withTime];
}

var maxQueryRad = 0.5 * PI / 180.0;
// When we're not looking at ALL the data, anything indexed should beat not-indexed.
var smallQuery = timeWithoutAndWithAnIndex({to: "2dsphere", from: "2dsphere"}, {
    from: {$within: {$centerSphere: [nyc.coordinates, maxQueryRad]}},
    to: {$within: {$centerSphere: [miami.coordinates, maxQueryRad]}}
});
print("Indexed time " + smallQuery[1] + " unindexed " + smallQuery[0]);
// assert(smallQuery[0] > smallQuery[1]);

// Let's just index one field.
var smallQuery = timeWithoutAndWithAnIndex({to: "2dsphere"}, {
    from: {$within: {$centerSphere: [nyc.coordinates, maxQueryRad]}},
    to: {$within: {$centerSphere: [miami.coordinates, maxQueryRad]}}
});
print("Indexed time " + smallQuery[1] + " unindexed " + smallQuery[0]);
// assert(smallQuery[0] > smallQuery[1]);

// And the other one.
var smallQuery = timeWithoutAndWithAnIndex({from: "2dsphere"}, {
    from: {$within: {$centerSphere: [nyc.coordinates, maxQueryRad]}},
    to: {$within: {$centerSphere: [miami.coordinates, maxQueryRad]}}
});
print("Indexed time " + smallQuery[1] + " unindexed " + smallQuery[0]);
// assert(smallQuery[0] > smallQuery[1]);
