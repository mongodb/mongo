var t = db.get_s2nearcomplex
t.drop()
t.ensureIndex({geo: "2dsphere"})

/* Short names for math operations */
Random.setRandomSeed();
var random = Random.rand;
var PI = Math.PI;
var asin = Math.asin;
var sin = Math.sin;
var cos = Math.cos;
var atan2 = Math.atan2


var originGeo = {type: "Point", coordinates: [20.0, 20.0]};
// Center point for all tests.
var origin = {
    name: "origin",
    geo: originGeo 
}


/*
 * Convenience function for checking that coordinates match.  threshold let's you
 * specify how accurate equals should be.
 */
function coordinateEqual(first, second, threshold){
    threshold = threshold || 0.001
    first = first['geo']['coordinates']
    second = second['geo']['coordinates']
    if(Math.abs(first[0] - second[0]) <= threshold){
        if(Math.abs(first[1] - second[1]) <= threshold){
            return true;
        }
    }
    return false;
}

/*
 * Creates `count` random and uniformly distributed points centered around `origin`
 * no points will be closer to origin than minDist, and no points will be further
 * than maxDist.  Points will be inserted into the global `t` collection, and will
 * be returned.
 * based on this algorithm: http://williams.best.vwh.net/avform.htm#LL
 */
function uniformPoints(origin, count, minDist, maxDist){
    var i;
    var lng = origin['geo']['coordinates'][0];
    var lat = origin['geo']['coordinates'][1];
    var distances = [];
    var points = [];
    for(i=0; i < count; i++){
        distances.push((random() * (maxDist - minDist)) + minDist);
    }
    distances.sort();
    while(points.length < count){
        var angle = random() * 2 * PI;
        var distance = distances[points.length];
        var pointLat = asin((sin(lat) * cos(distance)) + (cos(lat) * sin(distance) * cos(angle)));
        var pointDLng = atan2(sin(angle) * sin(distance) * cos(lat), cos(distance) - sin(lat) * sin(pointLat));
        var pointLng = ((lng - pointDLng + PI) % 2*PI) - PI;

        // Latitude must be [-90, 90]
        var newLat = lat + pointLat;
        if (newLat > 90) newLat -= 180;
        if (newLat < -90) newLat += 180;

        // Longitude must be [-180, 180]
        var newLng = lng + pointLng;
        if (newLng > 180) newLng -= 360;
        if (newLng < -180) newLng += 360;

        var newPoint = {
            geo: {
                type: "Point",
                //coordinates: [lng + pointLng, lat + pointLat]
                coordinates: [newLng, newLat]
            }
        };

        points.push(newPoint);
    }
    for(i=0; i < points.length; i++){
        t.insert(points[i]);
        assert(!db.getLastError());
    }
    return points;
}

/*
 * Creates a random uniform field as above, excepting for `numberOfHoles` gaps that
 * have `sizeOfHoles` points missing centered around a random point. 
 */
function uniformPointsWithGaps(origin, count, minDist, maxDist, numberOfHoles, sizeOfHoles){
    var points = uniformPoints(origin, count, minDist, maxDist);
    var i;
    for(i=0; i<numberOfHoles; i++){
        var randomPoint = points[Math.floor(random() * points.length)];
        removeNearest(randomPoint, sizeOfHoles);
    }
}

/*
 * Creates a random uniform field as above, expcepting for `numberOfClusters` clusters,
 * which will consist of N points where `minClusterSize` <= N <= `maxClusterSize.
 * you may specify an optional `distRatio` parameter which will specify the area that the cluster
 * covers as a fraction of the full area that points are created on.  Defaults to 10.
 */
function uniformPointsWithClusters(origin, count, minDist, maxDist, numberOfClusters, minClusterSize, maxClusterSize, distRatio){
    distRatio = distRatio || 10
    var points = uniformPoints(origin, count, minDist, maxDist);
    for(j=0; j<numberOfClusters; j++){
        var randomPoint = points[Math.floor(random() * points.length)];
        var clusterSize = (random() * (maxClusterSize - minClusterSize)) + minClusterSize;
        uniformPoints(randomPoint, clusterSize, minDist / distRatio, maxDist / distRatio);
    }
}
/*
 * Function used to create gaps in existing point field.  Will remove the `number` nearest
 * geo objects to the specified `point`.
 */
function removeNearest(point, number){
    var pointsToRemove = t.find({geo: {$geoNear: {$geometry: point['geo']}}}).limit(number);
    var idsToRemove = [];
    while(pointsToRemove.hasNext()){
        point = pointsToRemove.next();
        idsToRemove.push(point['_id']);
    }

    t.remove({_id: {$in: idsToRemove}});
}
/*
 * Validates the ordering of the nearest results is the same no matter how many 
 * geo objects are requested.  This could fail if two points have the same dist
 * from origin, because they may not be well-ordered.  If we see strange failures,
 * we should consider that.
 */
function validateOrdering(query){
    var near10 = t.find(query).limit(10);
    var near20 = t.find(query).limit(20);
    var near30 = t.find(query).limit(30);
    var near40 = t.find(query).limit(40);

    for(i=0;i<10;i++){
        assert(coordinateEqual(near10[i], near20[i]));
        assert(coordinateEqual(near10[i], near30[i]));
        assert(coordinateEqual(near10[i], near40[i]));
    }

    for(i=0;i<20;i++){
        assert(coordinateEqual(near20[i], near30[i]));
        assert(coordinateEqual(near20[i], near40[i]));
    }

    for(i=0;i<30;i++){
        assert(coordinateEqual(near30[i], near40[i]));
    }
}

var query = {geo: {$geoNear: {$geometry: originGeo}}};

// Test a uniform distribution of 10000 points.
uniformPoints(origin, 10000, 0.5, 1.5);

validateOrdering({geo: {$geoNear: {$geometry: originGeo}}})

print("Millis for uniform:")
print(t.find(query).explain().millis)
print("Total points:");
print(t.find(query).count());

t.drop()
t.ensureIndex({geo: "2dsphere"})
// Test a uniform distribution with 5 gaps each with 10 points missing.
uniformPointsWithGaps(origin, 10000, 1, 10.0, 5, 10);

validateOrdering({geo: {$geoNear: {$geometry: originGeo}}})

print("Millis for uniform with gaps:")
print(t.find(query).explain().millis)
print("Total points:");
print(t.find(query).count());

t.drop()
t.ensureIndex({geo: "2dsphere"})

// Test a uniform distribution with 5 clusters each with between 10 and 100 points.
uniformPointsWithClusters(origin, 10000, 1, 10.0, 5, 10, 100);

validateOrdering({geo: {$geoNear: {$geometry: originGeo}}})

print("Millis for uniform with clusters:");
print(t.find(query).explain().millis);
print("Total points:");
print(t.find(query).count());

t.drop()
t.ensureIndex({geo: "2dsphere"})

// Test a uniform near search with origin around the pole.

// Center point near pole.
originGeo = {type: "Point", coordinates: [0.0, 89.0]};
origin = {
    name: "origin",
    geo: originGeo 
}
uniformPoints(origin, 50, 0.5, 1.5);

validateOrdering({geo: {$geoNear: {$geometry: originGeo}}})

print("Millis for uniform near pole:")
print(t.find({geo: {$geoNear: {$geometry: originGeo}}}).explain().millis)
assert.eq(t.find({geo: {$geoNear: {$geometry: originGeo}}}).count(), 50);

t.drop()
t.ensureIndex({geo: "2dsphere"})

// Center point near the meridian
originGeo = {type: "Point", coordinates: [179.0, 0.0]};
origin = {
    name: "origin",
    geo: originGeo 
}
uniformPoints(origin, 50, 0.5, 1.5);

validateOrdering({geo: {$geoNear: {$geometry: originGeo}}})

print("Millis for uniform on meridian:")
print(t.find({geo: {$near: {$geometry: originGeo}}}).explain().millis)
assert.eq(t.find({geo: {$geoNear: {$geometry: originGeo}}}).count(), 50);

t.drop()
t.ensureIndex({geo: "2dsphere"})

// Center point near the negative meridian
originGeo = {type: "Point", coordinates: [-179.0, 0.0]};
origin = {
    name: "origin",
    geo: originGeo 
}
uniformPoints(origin, 50, 0.5, 1.5);

validateOrdering({geo: {$near: {$geometry: originGeo}}})

print("Millis for uniform on negative meridian:");
print(t.find({geo: {$near: {$geometry: originGeo}}}).explain().millis);
assert.eq(t.find({geo: {$near: {$geometry: originGeo}}}).count(), 50);

// Near search with points that are really far away.
t.drop()
t.ensureIndex({geo: "2dsphere"})
originGeo = {type: "Point", coordinates: [0.0, 0.0]};
origin = {
    name: "origin",
    geo: originGeo
}

uniformPoints(origin, 10, 89, 90);

cur = t.find({geo: {$near: {$geometry: originGeo}}})

assert.eq(cur.count(), 10);

print("Near search on very distant points:");
print(t.find({geo: {$near: {$geometry: originGeo}}}).explain().millis);
pt = cur.next();
assert(pt)
