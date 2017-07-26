(function() {
    var coll = db.SERVER_27968;
    coll.drop();

    // Basic tests
    assert.writeOK(coll.insert({name: "Point1", location: {type: "Point", coordinates: [1, 1]}}));
    assert.writeOK(coll.insert(
        {name: "LineString1", location: {type: "LineString", coordinates: [[1, 1], [2, 2]]}}));
    assert.writeOK(coll.insert({
        name: "Polygon1",
        location: {type: "Polygon", coordinates: [[[1, 1], [2, 2], [2, 1], [1, 1]]]}
    }));

    var result =
        coll.find({location: {$geoWithin: {$centerSphere: [[1, 1], 1]}}}, {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, [{name: 'Point1'}, {name: 'LineString1'}, {name: 'Polygon1'}]);

    // Test for a LineString within a geowithin sphere
    var route = {
        "name": "LineString2",
        "route": {
            "type": "LineString",
            "coordinates": [
                [151.0997772216797, -33.86157820443923],
                [151.21719360351562, -33.8952122494965]
            ]
        }
    };
    assert.writeOK(coll.insert(route));
    result =
        coll.find({
                route: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.16789425018004, -33.8508357122312], 0.0011167360027064348]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, [{name: "LineString2"}]);

    // Test for a LineString intersecting with geowithin sphere (should not return a match)
    result =
        coll.find({
                route: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.09822404831158, -33.85109290503663], 0.0013568277575574095]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, []);

    // Test for a LineString forming a closed loop rectangle within a geowithin sphere
    route = {
        "name": "LineString3",
        "route": {
            "type": "LineString",
            "coordinates": [
                [151.1986541748047, -33.85045895313796],
                [151.22835159301758, -33.85060151680233],
                [151.22800827026367, -33.87312358690302],
                [151.1989974975586, -33.87312358690302],
                [151.19813919067383, -33.845611646988544]
            ]
        }
    };
    assert.writeOK(coll.insert(route));
    result =
        coll.find({
                route: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.211603874292, -33.85773242760729], 0.00036551370187988725]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, [{name: "LineString3"}]);

    // Test for a LineString intersecting with geowithin sphere (should not return a match)
    result =
        coll.find({
                route: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.21456360474633, -33.86297699430086], 0.00031012015041612405]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, []);

    // Test for a Polygon within a geowithin sphere
    var city = {
        "name": "Polygon2",
        "city": "Wellington",
        "boundary": {
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
    assert.writeOK(coll.insert(city));
    result =
        coll.find({
                boundary: {
                    $geoWithin: {
                        $centerSphere:
                            [[174.78536621904806, -41.30510816038769], 0.0009483659386360411]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, [{name: "Polygon2"}]);

    // Test for a Polygon intersecting with geowithin sphere (should not return a match)
    result =
        coll.find({
                boundary: {
                    $geoWithin: {
                        $centerSphere:
                            [[174.7599527533759, -41.27137819591382], 0.0011247013153526434]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, []);

    // Test for a MultiPolygon (two seperate polygons) within a geowithin sphere
    city = {
        "name": "MultiPolygon1",
        "city": "Sydney",
        "boundary": {
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

    assert.writeOK(coll.insert(city));
    result =
        coll.find({
                boundary: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.20821632978107, -33.865139891361636], 0.000981007241416606]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, [{name: "MultiPolygon1"}]);

    // Test for a MultiPolygon with geowithin sphere only one of the polygons (should not return a
    // match)
    result =
        coll.find({
                boundary: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.20438542915883, -33.89006380099829], 0.0006390286437185907]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, []);

    // Test for a MultiPolygon (with a hole) within a geowithin sphere
    customRegion = {
        "name": "MultiPolygon2",
        "city": "Sydney",
        "boundary": {
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
    assert.writeOK(coll.insert(customRegion));
    result =
        coll.find({
                boundary: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.20936119647115, -33.875266834633265], 0.00020277354002627845]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, [{name: "MultiPolygon2"}]);

    // Test for a MultiPolygon with holes intersecting with geowithin sphere (should not return a
    // match)
    result =
        coll.find({
                boundary: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.21028000820485, -33.87067923462358], 0.00013138775245714733]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, []);

    // Test for a MultiPolygon with holes with geowithin sphere inside the hole (should not return a
    // match)
    result =
        coll.find({
                boundary: {
                    $geoWithin: {
                        $centerSphere:
                            [[151.21093787887645, -33.87533330567804], 0.000016565456776516003]
                    }
                }
            },
                  {"name": 1, "_id": 0})
            .toArray();
    assert.eq(result, []);

    coll.drop();
})();
