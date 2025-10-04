/**
 * Use of GeoJSON points should be prohibited with a 2d index, SERVER-10636.
 */

let t = db.geo_2d_with_geojson_point;
t.drop();
t.createIndex({loc: "2d"});

let geoJSONPoint = {type: "Point", coordinates: [0, 0]};

print(
    assert.throws(
        function () {
            t.findOne({loc: {$near: {$geometry: geoJSONPoint}}});
        },
        [],
        "querying 2d index with GeoJSON point.",
    ),
);
