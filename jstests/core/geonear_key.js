/**
 * Tests for the 'key' field accepted by the $geoNear aggregation stage.
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    const coll = db.jstests_geonear_key;
    coll.drop();

    assert.writeOK(coll.insert({_id: 0, a: [1, 1]}));
    assert.writeOK(coll.insert({_id: 1, a: [1, 2]}));
    assert.writeOK(coll.insert({_id: 2, b: {c: [1, 1]}}));
    assert.writeOK(coll.insert({_id: 3, b: {c: [1, 2]}}));
    assert.writeOK(coll.insert({_id: 4, b: {d: [1, 1]}}));
    assert.writeOK(coll.insert({_id: 5, b: {d: [1, 2]}}));

    /**
     * Runs an aggregation consisting of a single $geoNear stage described by 'nearParams', and
     * returns the raw command result object. 'nearParams' consists of the parameters to the
     * $geoNear stage, but is expected to omit 'distanceField'.
     */
    function runNearAgg(nearParams) {
        let nearAggParams = Object.extend({distanceField: "dist"}, nearParams);
        let nearAggStage = {$geoNear: nearAggParams};
        let aggCmd = {aggregate: coll.getName(), pipeline: [nearAggStage], cursor: {}};
        return db.runCommand(aggCmd);
    }

    /**
     * Runs the near described by 'nearParams' as a $geoNear aggregation and verifies that the
     * operation fails with 'code'.
     */
    function assertGeoNearFails(nearParams, code) {
        assert.commandFailedWithCode(runNearAgg(nearParams), code);
    }

    /**
     * Runs the near described by 'nearParams' as a $geoNear aggregation and verifies that the
     * operation returns the _id values in 'expectedIds', in order.
     */
    function assertGeoNearSucceedsAndReturnsIds(nearParams, expectedIds) {
        let aggResult = assert.commandWorked(runNearAgg(nearParams));
        let res = aggResult.cursor.firstBatch;
        let errfn = () => `expected ids ${tojson(expectedIds)}, but these documents were ` +
            `returned: ${tojson(res)}`;

        assert.eq(expectedIds.length, res.length, errfn);
        for (let i = 0; i < expectedIds.length; i++) {
            assert.eq(expectedIds[i], aggResult.cursor.firstBatch[i]._id, errfn);
        }
    }

    // Verify that $geoNear fails when the key field is not a string.
    assertGeoNearFails({near: [0, 0], key: 1}, ErrorCodes.TypeMismatch);

    // Verify that $geoNear fails when the key field the empty string.
    assertGeoNearFails({near: [0, 0], key: ""}, ErrorCodes.BadValue);

    // Verify that $geoNear fails when there are no eligible indexes.
    assertGeoNearFails({near: [0, 0]}, ErrorCodes.IndexNotFound);

    // Verify that the query system raises an error when an index is specified that doesn't exist.
    assertGeoNearFails({near: [0, 0], key: "a"}, ErrorCodes.BadValue);

    // Create a number of 2d and 2dsphere indexes.
    assert.commandWorked(coll.createIndex({a: "2d"}));
    assert.commandWorked(coll.createIndex({a: "2dsphere"}));
    assert.commandWorked(coll.createIndex({"b.c": "2d"}));
    assert.commandWorked(coll.createIndex({"b.d": "2dsphere"}));

    // Verify that $geoNear fails when the index to use is ambiguous because of the absence of the
    // key field.
    assertGeoNearFails({near: [0, 0]}, ErrorCodes.IndexNotFound);

    // Verify that the key field can correctly identify the index to use, when there is only a
    // single geo index on the relevant path.
    assertGeoNearSucceedsAndReturnsIds({near: [0, 0], key: "b.c"}, [2, 3]);
    assertGeoNearSucceedsAndReturnsIds({near: {type: "Point", coordinates: [0, 0]}, key: "b.d"},
                                       [4, 5]);

    // Verify that when the key path has both a 2d or 2dsphere index, the command still succeeds.
    assertGeoNearSucceedsAndReturnsIds({near: [0, 0], key: "a"}, [0, 1]);
    assertGeoNearSucceedsAndReturnsIds({near: [0, 0], spherical: true, key: "a"}, [0, 1]);
    assertGeoNearSucceedsAndReturnsIds({near: {type: "Point", coordinates: [0, 0]}, key: "a"},
                                       [0, 1]);
    assertGeoNearSucceedsAndReturnsIds(
        {near: {type: "Point", coordinates: [0, 0]}, spherical: true, key: "a"}, [0, 1]);

    // Verify that $geoNear fails when a GeoJSON point is used with a 'key' path that only has a 2d
    // index. GeoJSON points can only be used for spherical geometry.
    assertGeoNearFails({near: {type: "Point", coordinates: [0, 0]}, key: "b.c"},
                       ErrorCodes.BadValue);

    // Verify that $geoNear fails when:
    //  -- The only index available over the 'key' path is 2dsphere.
    //  -- spherical=false.
    //  -- The search point is a legacy coordinate pair.
    assertGeoNearFails({near: [0, 0], key: "b.d"}, ErrorCodes.BadValue);
    assertGeoNearFails({near: [0, 0], key: "b.d", spherical: false}, ErrorCodes.BadValue);
}());
