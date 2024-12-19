/*
 * Tests the behavior of the $geoNear stage with varying expression values of 'minDistance' and
 * 'maxDistance'.
 * @tags: [
 *   requires_pipeline_optimization,
 *   # $geoNear is not allowed in a facet.
 *   do_not_wrap_aggregations_in_facets,
 *   requires_fcv_72
 * ]
 */

const coll = db.getCollection("geonear_mindistance_maxdistance");

const kMaxDistance = Math.PI * 2.0;

// Test points that are exactly at the "near" point, close to the point, and far from the point.
// Distances are purposely chosen to be small so that distances in meters and radians are close.
const origin = {
    pt: [0, 0]
};
const near = {
    pt: [0.23, -0.32]
};
const far = {
    pt: [5.9, 0.0]
};

["2d", "2dsphere"].forEach(geoType => {
    jsTestLog(`Testing $geoNear with index {pt: "${geoType}"}`);
    coll.drop();

    // Create the desired index type and populate the collection.
    assert.commandWorked(coll.createIndex({pt: geoType}));
    [origin, near, far].forEach(doc => {
        doc.distFromOrigin = (geoType === "2dsphere") ? Geo.sphereDistance(doc.pt, origin.pt)
                                                      : Geo.distance(doc.pt, origin.pt);
        assert.commandWorked(coll.insert(doc));
    });

    /**
     * Helper function that runs a $geoNear aggregation near the origin, setting the minimum
     * and/or maximum search distance using the object 'minMaxOpts', and asserting that the
     * results match 'expected'.
     */
    function assertGeoNearResults(minMaxOpts, expected) {
        const geoNearStage = {
            $geoNear: Object.extend(
                {near: origin.pt, distanceField: "dist", spherical: (geoType === "2dsphere")},
                minMaxOpts)
        };
        const projStage = {$project: {_id: 0, dist: 0}};
        const res = coll.aggregate([geoNearStage, projStage]).toArray();
        assert.eq(res,
                  expected,
                  () => `Unexpected results from ${tojson(geoNearStage)} using a ${geoType} index`);
    }

    function compareConstVersusExpression(minOrMax, value) {
        const pipeline = [{
            $geoNear: {
                near: origin.pt,
                distanceField: "dist",
                spherical: (geoType === "2dsphere"),
                [minOrMax]: "$$val"
            }
        }];
        const res = coll.aggregate(pipeline, {let : {val: value}}).toArray();
        const constRes = coll.aggregate([{
                                 $geoNear: {
                                     near: origin.pt,
                                     distanceField: "dist",
                                     spherical: (geoType === "2dsphere"),
                                     [minOrMax]: value
                                 }
                             }])
                             .toArray();

        assert.eq(res.length, constRes.length);
        for (let i = 0; i < res.length; i++) {
            assert.eq(res[i].distance, constRes[i].distance);
        }
    }

    // Non-numeric and non-constant expressions for minDistance are illegal.
    assert.throwsWithCode(() => assertGeoNearResults({minDistance: {$concat: ["3", ".2"]}}),
                          ErrorCodes.TypeMismatch);
    assert.throwsWithCode(() => assertGeoNearResults({minDistance: {$getField: "distFromOrigin"}}),
                          7555701);
    assert.throwsWithCode(() => assertGeoNearResults({minDistance: "$distFromOrigin"}), 7555701);

    // Compare results from const and variable minDistance.
    compareConstVersusExpression("minDistance", 0.0);
    compareConstVersusExpression("minDistance", (near.distFromOrigin / 2));

    // Minimum distance can be an expression.
    assertGeoNearResults({minDistance: {$add: [(near.distFromOrigin / 2), 1.0, -1.0]}},
                         [near, far]);

    // Non-numeric and non-constant expressions for maxDistance are illegal.
    assert.throwsWithCode(() => assertGeoNearResults({maxDistance: {$concat: ["3", ".2"]}}),
                          ErrorCodes.TypeMismatch);
    assert.throwsWithCode(() => assertGeoNearResults({maxDistance: {$getField: "distFromOrigin"}}),
                          7555702);
    assert.throwsWithCode(() => assertGeoNearResults({maxDistance: "$distFromOrigin"}), 7555702);

    // Compare results from const and variable maxDistance.
    compareConstVersusExpression("maxDistance", 0.0);
    compareConstVersusExpression("maxDistance", (near.distFromOrigin / 2));

    // Test minDistance and maxDistance as expressions.
    assertGeoNearResults(
        {minDistance: {$add: [1.0, -1.0]}, maxDistance: {$multiply: [1.0, kMaxDistance]}},
        [origin, near, far]);

    function getLookupRes(minOrMax, lookupCollection) {
        const pipeline =[
            {$lookup: {
                    from: coll.getName(),
                    let: {val: "$" + minOrMax},
                    pipeline: [{$geoNear: {near: origin.pt ,distanceField: "dist", spherical:(geoType === "2dsphere"), [minOrMax]: "$$val"}},{$project: {_id: 0, dist: 0}}],
                    as: "output"
            }},{$sort : {_id: 1}}];

        const lookupRes = lookupCollection.aggregate(pipeline).toArray();
        return lookupRes;
    }

    function runLookupTest() {
        const coll2 = db.getCollection("store_min_max_values");
        coll2.drop();
        assert.commandWorked(coll2.insert({_id: 0, minDistance: 0.0, maxDistance: kMaxDistance}));
        assert.commandWorked(coll2.insert({
            _id: 1,
            minDistance: (near.distFromOrigin / 2),
            maxDistance: (near.distFromOrigin + 0.01)
        }));

        const minLookupRes = getLookupRes("minDistance", coll2);
        assert.eq(minLookupRes.length, 2);
        assert.eq(minLookupRes[0].output, [origin, near, far]);
        assert.eq(minLookupRes[1].output, [near, far]);

        const maxLookupRes = getLookupRes("maxDistance", coll2);
        assert.eq(maxLookupRes.length, 2);
        assert.eq(maxLookupRes[0].output, [origin, near, far]);
        assert.eq(maxLookupRes[1].output, [origin, near]);
    }
    runLookupTest();
});