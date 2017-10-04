// Tests for $geowithin $centerSphere operator with LineString and Polygon.
(function() {

    function testGeoWithinCenterSphereLinePolygon(coll) {
        coll.drop();

        // Convenient test function for $geowithin $centerSphere.
        function testGeoWithinCenterSphere(centerSphere, expected) {
            let result = coll.find({geoField: {$geoWithin: {$centerSphere: centerSphere}}},
                                   {"name": 1, "_id": 0})
                             .sort({"name": 1})
                             .toArray();
            assert.eq(result, expected);
        }

        // Basic tests.
        assert.writeOK(
            coll.insert({name: "Point1", geoField: {type: "Point", coordinates: [1, 1]}}));
        assert.writeOK(coll.insert(
            {name: "LineString1", geoField: {type: "LineString", coordinates: [[1, 1], [2, 2]]}}));
        assert.writeOK(coll.insert({
            name: "Polygon1",
            geoField: {type: "Polygon", coordinates: [[[1, 1], [2, 2], [2, 1], [1, 1]]]}
        }));

        // The second parameter of $centerSphere is in radian and the angle between [1, 1] and [2,2]
        // is about 0.0246 radian, much less than 1.
        testGeoWithinCenterSphere([[1, 1], 1],
                                  [{name: 'LineString1'}, {name: 'Point1'}, {name: 'Polygon1'}]);

        let geoDoc = {
            "name": "LineString2",
            "geoField": {
                "type": "LineString",
                "coordinates": [
                    [151.0997772216797, -33.86157820443923],
                    [151.21719360351562, -33.8952122494965]
                ]
            }
        };
        assert.writeOK(coll.insert(geoDoc));

        // Test for a LineString within a geowithin sphere.
        testGeoWithinCenterSphere([[151.16789425018004, -33.8508357122312], 0.0011167360027064348],
                                  [{name: "LineString2"}]);

        // Test for a LineString intersecting with geowithin sphere (should not return a match).
        testGeoWithinCenterSphere([[151.09822404831158, -33.85109290503663], 0.0013568277575574095],
                                  []);

        geoDoc = {
            "name": "LineString3",
            "geoField": {
                "type": "LineString",
                "coordinates": [
                    [174.72896575927734, -36.86698689106876],
                    [174.72965240478516, -36.90707799098374],
                    [174.7808074951172, -36.9062544131224],
                    [174.77840423583982, -36.88154294352893],
                    [174.72827911376953, -36.88373984256185]
                ]
            }
        };
        assert.writeOK(coll.insert(geoDoc));

        // Test for a LineString forming a closed loop rectangle within a geowithin sphere.
        testGeoWithinCenterSphere([[174.75211152791763, -36.88962755605813], 0.000550933650273084],
                                  [{name: "LineString3"}]);

        // Test for a LineString intersecting with geowithin sphere (should not return a match).
        testGeoWithinCenterSphere([[174.75689891704758, -36.8998373317427], 0.0005315628331256537],
                                  []);

        // Test for a LineString outside of geowithin sphere (should not return a match).
        testGeoWithinCenterSphere([[174.8099591465865, -36.89409450096385], 0.00027296698925637807],
                                  []);

        // Test for a Polygon within a geowithin sphere.
        geoDoc = {
            "name": "Polygon2",
            "city": "Wellington",
            "geoField": {
                "type": "Polygon",
                "coordinates": [[
                    [174.72930908203125, -41.281676559981676],
                    [174.76261138916013, -41.34820622928743],
                    [174.84329223632812, -41.32861539747227],
                    [174.8312759399414, -41.280902559820895],
                    [174.72930908203125, -41.281676559981676]
                ]]
            }
        };
        assert.writeOK(coll.insert(geoDoc));

        // Test for a Polygon within a geowithin sphere.
        testGeoWithinCenterSphere([[174.78536621904806, -41.30510816038769], 0.0009483659386360411],
                                  [{name: "Polygon2"}]);

        // Test for an empty query cap (radius 0) inside of a polygon that covers the centerSphere
        // (should not return a match).
        testGeoWithinCenterSphere([[174.79144274337722, -41.307682001033385], 0], []);

        // Test for a Polygon intersecting with geowithin sphere (should not return a match).
        testGeoWithinCenterSphere([[174.7599527533759, -41.27137819591382], 0.0011247013153526434],
                                  []);

        // Test for a Polygon outside of geowithin sphere (should not return a match).
        testGeoWithinCenterSphere(
            [[174.80008799649448, -41.201484845543426], 0.0007748581633291528], []);

        geoDoc = {
            "name": "MultiPolygon1",
            "city": "Sydney",
            "geoField": {
                "type": "MultiPolygon",
                "coordinates": [
                    [[
                       [151.21032714843747, -33.85074408022877],
                       [151.23367309570312, -33.84333046657819],
                       [151.20929718017578, -33.81680727566872],
                       [151.1876678466797, -33.829927301798676],
                       [151.21032714843747, -33.85074408022877]
                    ]],
                    [[
                       [151.20140075683594, -33.856446422184305],
                       [151.17565155029297, -33.88979749364442],
                       [151.2044906616211, -33.9151583833889],
                       [151.23058319091797, -33.87041555094182],
                       [151.20140075683594, -33.856446422184305]
                    ]]
                ]
            }
        };

        assert.writeOK(coll.insert(geoDoc));

        // Test for a MultiPolygon (two seperate polygons) within a geowithin sphere.
        testGeoWithinCenterSphere([[151.20821632978107, -33.865139891361636], 0.000981007241416606],
                                  [{name: "MultiPolygon1"}]);

        // Verify that only one of the polygons of a MultiPolygon in the $centerSphere does not
        // match
        testGeoWithinCenterSphere([[151.20438542915883, -33.89006380099829], 0.0006390286437185907],
                                  []);

        geoDoc = {
            "name": "MultiPolygon2",
            "city": "Sydney",
            "geoField": {
                "type": "MultiPolygon",
                "coordinates": [[
                    [
                      [151.203031539917, -33.87116383262648],
                      [151.20401859283447, -33.88270791866475],
                      [151.21891021728516, -33.88256540860479],
                      [151.2138032913208, -33.86817066653049],
                      [151.203031539917, -33.87116383262648]
                    ],
                    [
                      [151.21041297912598, -33.86980979429744],
                      [151.20938301086426, -33.8767579211837],
                      [151.2121295928955, -33.87722110953139],
                      [151.21315956115723, -33.86995232565932],
                      [151.21041297912598, -33.86980979429744]
                    ]
                ]]
            }
        };
        assert.writeOK(coll.insert(geoDoc));

        // Test for a MultiPolygon (with a hole) within a geowithin sphere.
        testGeoWithinCenterSphere(
            [[151.20936119647115, -33.875266834633265], 0.00020277354002627845],
            [{name: "MultiPolygon2"}]);

        // Test for centerSphere as big as earth radius (should return all).
        testGeoWithinCenterSphere(
            [[151.20936119647115, -33.875266834633265], 3.14159265358979323846], [
                {name: "LineString1"},
                {name: "LineString2"},
                {name: "LineString3"},
                {name: "MultiPolygon1"},
                {name: "MultiPolygon2"},
                {name: "Point1"},
                {name: "Polygon1"},
                {name: "Polygon2"}
            ]);

        // Test for a MultiPolygon with holes intersecting with geowithin sphere (should not return
        // a match).
        testGeoWithinCenterSphere(
            [[151.21028000820485, -33.87067923462358], 0.00013138775245714733], []);

        // Test for a MultiPolygon with holes with geowithin sphere inside the hole (should not
        // return a match).
        testGeoWithinCenterSphere(
            [[151.21093787887645, -33.87533330567804], 0.000016565456776516003], []);

        coll.drop();

        // Test for a large query cap containing both of line vertices but not the line itself.
        // (should not return a match).
        geoDoc = {
            "name": "HorizontalLongLine",
            "geoField": {
                "type": "LineString",
                "coordinates": [[96.328125, 5.61598581915534], [153.984375, -6.315298538330033]]
            }
        };
        assert.writeOK(coll.insert(geoDoc));

        // Test for a large query cap containing both of line vertices but not the line itself.
        // (should not return a match).
        testGeoWithinCenterSphere([[-59.80246852929814, -2.3633072488322853], 2.768403272464979],
                                  []);

        coll.drop();

        // Test for a large query cap containing all polygon vertices but not the whole polygon.
        // (should not return a match).
        geoDoc = {
            "name": "LargeRegion",
            "geoField": {
                "type": "Polygon",
                "coordinates": [[
                    [98.96484375, -11.350796722383672],
                    [135.35156249999997, -11.350796722383672],
                    [135.35156249999997, 0.8788717828324276],
                    [98.96484375, 0.8788717828324276],
                    [98.96484375, -11.350796722383672]
                ]]
            }
        };
        assert.writeOK(coll.insert(geoDoc));

        // Test for a large query cap containing both of line vertices but not the line itself.
        // (should not return a match).
        testGeoWithinCenterSphere([[-61.52266094410311, 17.79937981451866], 2.9592242752161573],
                                  []);
    }

    // Test $geowithin $centerSphere for LineString and Polygon without index.
    let coll = db.geo_s2within_line_polygon_sphere;
    testGeoWithinCenterSphereLinePolygon(coll);

    // Test $geowithin $centerSphere for LineString and Polygon with 2dsphere index.
    assert.commandWorked(coll.createIndex({geoField: "2dsphere"}));
    testGeoWithinCenterSphereLinePolygon(coll);
})();