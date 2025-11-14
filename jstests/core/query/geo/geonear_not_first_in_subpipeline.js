/**
 * Test that having a $lookup subpipeline with $geoNear not as the first stage after cache
 * optimization fails the query during validation instead of execution. This is intended to
 * reproduce AF-7237.
 * @tags: [requires_fcv_83]
 */

const coll = db.geonear_not_first_in_subpipeline;
const targetColl = db.geonear_not_first_in_subpipeline_target;
coll.drop();
targetColl.drop();

assert.commandWorked(coll.insert({_id: 1}));
assert.commandWorked(targetColl.insert({_id: 1}));

assert.commandFailedWithCode(
    coll.runCommand({
        aggregate: coll.getName(),
        pipeline: [
            {
                "$lookup": {
                    "from": targetColl.getName(),
                    "as": "result",
                    "pipeline": [
                        {"$limit": 1},
                        {
                            "$geoNear": {"near": {"type": "Point", "coordinates": [0, 0]}, "distanceField": "dist"},
                        },
                    ],
                },
            },
        ],
        cursor: {},
    }),
    40603,
);
