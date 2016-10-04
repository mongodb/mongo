// Tests upgrade/downgrade between 2dsphere index versions 2 and 3

// @tags: [requires_mmapv1]

function generatePoint() {
    var longitude = Math.random() * 10 - 5;
    var latitude = Math.random() * 10 - 5;
    var pt = {geometry: {type: "Point", coordinates: [longitude, latitude]}};
    return pt;
}

// Generates random points in lat/lon square
// of size 10x10 centered at [0,0]
function generatePoints(amount) {
    var points = [];
    for (var i = 0; i < amount; i++) {
        points.push(generatePoint());
    }
    return points;
}

function generatePolygons(amount) {
    var polygons = [];
    for (var i = 0; i < amount; i++) {
        var numpoints = 4 + Math.floor(Math.random() * 10);
        var dist = Math.random() * 5 + .01;
        var coordinates = [];
        for (var j = 0; j < numpoints - 1; j++) {
            var angle = (j / numpoints) * 2 * Math.PI;
            coordinates.push([dist * Math.cos(angle), dist * Math.sin(angle)]);
        }
        coordinates.push(coordinates[0]);
        polygons.push({geometry: {type: "Polygon", coordinates: [coordinates]}});
    }
    return polygons;
}

function getCollection(conn) {
    return conn.getDB("test").twoDSphereVersion;
}

function get2dsphereIndexVersion(coll) {
    var indexes = coll.getIndexes();
    for (var i = 0; i < indexes.length; i++) {
        if (indexes[i].name == "geometry_2dsphere") {
            return indexes[i]["2dsphereIndexVersion"];
        }
    }
    return -1;
}

var nearQuery = {geometry: {$near: {$geometry: {type: "Point", coordinates: [0, 0]}}}};

// SERVER-25129 - Only runs in mmapv1
var storageEngine = "mmapv1";
var mongod = MongoRunner.runMongod({binVersion: "3.0", storageEngine: storageEngine});
var coll = getCollection(mongod);
var res = coll.insert(generatePoints(10));
res = coll.insert(generatePolygons(10));
res = coll.createIndex({geometry: "2dsphere"}, {"2dsphereIndexVersion": 2});
assert.eq(2, get2dsphereIndexVersion(coll));
res = coll.find(nearQuery);
assert.eq(res.itcount(), 20);

// Version 2 index should still work fine in latest
MongoRunner.stopMongod(mongod);
mongod =
    MongoRunner.runMongod({binVersion: "latest", restart: mongod, storageEngine: storageEngine});
coll = getCollection(mongod);
assert.eq(2, get2dsphereIndexVersion(coll));
res = coll.find(nearQuery);
assert.eq(res.itcount(), 20);

// reindex to version 3
coll.dropIndex({geometry: "2dsphere"});
res = coll.createIndex({geometry: "2dsphere"}, {"2dsphereIndexVersion": 3});
assert.eq(3, get2dsphereIndexVersion(coll));
res = coll.find(nearQuery);
assert.eq(res.itcount(), 20);

// downgrading shouldn't be able to startup because of assertion error
MongoRunner.stopMongod(mongod);
var failed_mongod =
    MongoRunner.runMongod({binVersion: "3.0", restart: mongod, storageEngine: storageEngine});
assert.eq(failed_mongod, null);

// upgrade, reindex, then downgrade to fix
mongod =
    MongoRunner.runMongod({binVersion: "latest", restart: mongod, storageEngine: storageEngine});
coll = getCollection(mongod);
assert.eq(3, get2dsphereIndexVersion(coll));
res = coll.dropIndex({geometry: "2dsphere"});
res = coll.createIndex({geometry: "2dsphere"}, {"2dsphereIndexVersion": 2});
assert.eq(2, get2dsphereIndexVersion(coll));
MongoRunner.stopMongod(mongod);
mongod = MongoRunner.runMongod({binVersion: "3.0", restart: mongod, storageEngine: storageEngine});
assert.neq(mongod, null);
coll = getCollection(mongod);
assert.eq(2, get2dsphereIndexVersion(coll));
res = coll.find(nearQuery);
assert.eq(res.itcount(), 20);