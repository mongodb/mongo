//
// Test input validation for $near and $nearSphere queries.
//
var t = db.geo_query_input_validation;

// The test matrix. Some combinations are not supported:
//     2d       index and $minDistance.
//     2dsphere index, $near, and legacy coordinates.
var indexTypes = ['2d', '2dsphere'],
    queryTypes = ['$near', '$nearSphere'],
    pointTypes = [
        {$geometry: {type: 'Point', coordinates: [0, 0]}},
        [0, 0]],
    optionNames = ['$minDistance', '$maxDistance'],
    badDistances = [-1, undefined, 'foo'];

indexTypes.forEach(function(indexType) {
    t.drop();
    t.createIndex({'loc': indexType});

    queryTypes.forEach(function(queryType) {
        pointTypes.forEach(function(pointType) {
            optionNames.forEach(function(optionName) {
                var isLegacy = Array.isArray(pointType),
                    pointDescription = (isLegacy ? "legacy coordinates" : "GeoJSON point");

                // Like {loc: {$nearSphere: [0, 0], $maxDistance: 1}}.
                var query = {};
                query[queryType] = pointType;
                query[optionName] = 1;

                var locQuery = {loc: query};

                if (indexType == '2d' && !isLegacy) {
                    // Currently doesn't throw errors but also doesn't work as
                    // expected: SERVER-10636. Stop processing this combination.
                    return;
                }

                // Unsupported combinations should return errors.
                if (
                    (indexType == '2d' && optionName == '$minDistance') ||
                    (indexType == '2dsphere' && queryType == '$near' && isLegacy)
                ) {
                    assert.throws(
                        function() {
                            t.find(locQuery).itcount();
                        },
                        [],
                        queryType + " query with " + indexType
                            + " index and " + pointDescription
                            + " should've failed."
                    );

                    // Stop processing this combination in the test matrix.
                    return;
                }

                // This is a supported combination. No error.
                t.find(locQuery).itcount();

                function makeQuery(distance) {
                    // Like {$nearSphere: geoJSONPoint, $minDistance: -1}.
                    var query = {};
                    query[queryType] = pointType;
                    query[optionName] = distance;
                    return query;
                }

                function doQuery(query) {
                    t.find({loc: query}).itcount();
                }

                // No error with $min/$maxDistance 1.
                doQuery(makeQuery(1));

                var outOfRangeDistances = [];
                if (indexType == '2d' && queryType == '$near') {
                    // $maxDistance unlimited; no error.
                    doQuery(makeQuery(1e10));
                } else if (isLegacy) {
                    // Radians can't be more than pi.
                    outOfRangeDistances.push(Math.PI + 0.1);
                } else {
                    // $min/$maxDistance is in meters, so distances greater
                    // than pi are ok, but not more than half earth's
                    // circumference in meters.
                    doQuery(makeQuery(Math.PI + 0.1));

                    var earthRadiusMeters = 6378.1 * 1000;
                    outOfRangeDistances.push(Math.PI * earthRadiusMeters + 100);
                }

                // Try several bad values for $min/$maxDistance.
                badDistances.concat(outOfRangeDistances).forEach(function(badDistance) {

                    var msg = (
                        queryType + " with " + pointDescription
                            + " and " + indexType + " index should've failed with "
                            + optionName + " " + badDistance);

                    assert.throws(doQuery, [makeQuery(badDistance)], msg);
                });
            });
        });
    });
});
