//
// Test input validation for geoNear command.
//
let t = db.geonear_cmd_input_validation;
t.drop();
t.createIndex({loc: "2dsphere"});

// The test matrix. Some combinations are not supported:
//     2d index and GeoJSON.
//     2dsphere index with legacy coordinate pair and spherical=false.
let indexTypes = ["2d", "2dsphere"],
    pointTypes = [{type: "Point", coordinates: [0, 0]}, [0, 0]],
    sphericalOptions = [true, false],
    optionNames = ["minDistance", "maxDistance"],
    badNumbers = [-1, undefined, "foo"],
    unknownArg = "foo";

indexTypes.forEach(function (indexType) {
    t.drop();
    t.createIndex({"loc": indexType});

    pointTypes.forEach(function (pointType) {
        sphericalOptions.forEach(function (spherical) {
            optionNames.forEach(function (optionName) {
                let isLegacy = Array.isArray(pointType),
                    pointDescription = isLegacy ? "legacy coordinates" : "GeoJSON point";

                function makeCommand(distance) {
                    let geoNearSpec = {
                        near: pointType,
                        distanceField: "dist",
                        spherical: spherical,
                    };
                    geoNearSpec[optionName] = distance;

                    return {
                        aggregate: t.getName(),
                        cursor: {},
                        pipeline: [{$geoNear: geoNearSpec}],
                    };
                }

                // Unsupported combinations should return errors.
                if ((indexType == "2d" && !isLegacy) || (indexType == "2dsphere" && isLegacy && !spherical)) {
                    assert.commandFailed(
                        db.runCommand(makeCommand(1)),
                        "geoNear with spherical=" +
                            spherical +
                            " and " +
                            indexType +
                            " index and " +
                            pointDescription +
                            " should've failed.",
                    );

                    // Stop processing this combination in the test matrix.
                    return;
                }

                // Test that there is no error when not specifying a min or max distance.
                assert.commandWorked(
                    db.runCommand({
                        aggregate: t.getName(),
                        cursor: {},
                        pipeline: [{$geoNear: {near: pointType, distanceField: "dist", spherical: spherical}}],
                    }),
                );

                // No error with min/maxDistance 1.
                assert.commandWorked(db.runCommand(makeCommand(1)));

                let outOfRangeDistances = [];
                if (indexType == "2d") {
                    // maxDistance unlimited; no error.
                    assert.commandWorked(db.runCommand(makeCommand(1e10)));
                }

                // Try several bad values for min/maxDistance.
                badNumbers.concat(outOfRangeDistances).forEach(function (badDistance) {
                    let msg =
                        "geoNear with spherical=" +
                        spherical +
                        " and " +
                        pointDescription +
                        " and " +
                        indexType +
                        " index should've failed with " +
                        optionName +
                        " " +
                        badDistance;

                    assert.commandFailed(db.runCommand(makeCommand(badDistance)), msg);
                });

                // Bad values for limit / num.
                ["num", "limit"].forEach(function (limitOptionName) {
                    [-1, "foo"].forEach(function (badLimit) {
                        let msg =
                            "geoNear with spherical=" +
                            spherical +
                            " and " +
                            pointDescription +
                            " and " +
                            indexType +
                            " index should've failed with '" +
                            limitOptionName +
                            "' " +
                            badLimit;

                        let command = makeCommand(1);
                        command[limitOptionName] = badLimit;
                        assert.commandFailed(db.runCommand(command), msg);
                    });
                });

                // Bad values for distanceMultiplier.
                badNumbers.forEach(function (badNumber) {
                    let msg =
                        "geoNear with spherical=" +
                        spherical +
                        " and " +
                        pointDescription +
                        " and " +
                        indexType +
                        " index should've failed with distanceMultiplier " +
                        badNumber;

                    let command = makeCommand(1);
                    command["distanceMultiplier"] = badNumber;
                    assert.commandFailed(db.runCommand(command), msg);
                });

                // Unknown argument
                let msg = "geoNear should've failed with unknown arg " + unknownArg;
                let command = makeCommand(1);
                command[unknownArg] = "unknown";
                assert.commandFailed(db.runCommand(command), msg);
            });
        });
    });
});
