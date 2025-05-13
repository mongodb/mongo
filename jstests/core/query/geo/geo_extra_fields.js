// This test verifies that queries on GeoJSON work regardless of extra fields and their
// position in respect to the GeoJSON fields.
// @tags: [
//   requires_fcv_82
// ]

const testDB = db.geo_extra_fields;
testDB.drop();

const coords = [-95.90898701416783, -78.02003547216432];

// The documents have an identical 'be' object, aside from the presence or position of 'ua' which is
// an extra field unrelated to GeoJSON or the query. Its presence should not effect the correctness
// of $geoWithin.
testDB.insertMany([
    {
        "_id": "65a04bdf95d67a5d1230b4c3",
        "be": {"ua": 209268.984375, "coordinates": coords, "type": "Point"}
    },
    {
        "_id": "65a04c0895d67a5d1230b4c4",
        "be": {"coordinates": coords, "type": "Point", "ua": 209268.984375}
    },
    {
        "_id": "65a04fae95d67a5d1230b4c5",
        "be": {"coordinates": coords, "ua": 209268.984375, "type": "Point"}
    },
    {"_id": "65a04bdf95d67a5d1230b4c6", "be": {"coordinates": coords, "type": "Point"}},

]);

// The positioning of the key value pairs in the document should not matter.
assert.eq(
    4,
    testDB
        .find({
            $or: [{
                "be": {
                    $geoWithin: {
                        $centerSphere: [[179.03531532305314, 0.7050703681776871], 2.145221550434281]
                    }
                }
            }]
        })
        .itcount());

assert.eq(
    4,
    testDB
        .find({$or: [{"be": {$geoIntersects: {$geometry: {type: 'Point', coordinates: coords}}}}]})
        .itcount());

assert.commandWorked(testDB.createIndex({be: "2dsphere"}));
assert.eq(4,
          testDB
              .aggregate([{
                  $geoNear: {
                      near: {type: "Point", coordinates: coords},
                      distanceField: "dist",
                      spherical: true,
                      key: "be",
                  }
              }])
              .itcount());