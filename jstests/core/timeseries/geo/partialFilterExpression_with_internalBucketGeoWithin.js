/**
 * This tests that partialFilterExpressions can include the $internalBucketGeoWithin operator when
 * indexing buckets of timeseries operators.
 * @tags: [
 *   # Refusing to run a test that issues an aggregation command with explain because it may
 *   # return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   requires_non_retryable_writes,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

import {
    createRawTimeseriesIndex,
    getTimeseriesCollForRawOps,
    kRawOperationSpec,
} from "jstests/core/libs/raw_operation_utils.js";
import {isShardedTimeseries} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";
import {getWinningPlanFromExplain, isCollscan, isIxscan} from "jstests/libs/query/analyze_plan.js";
import {IndexBuildTest} from "jstests/noPassthrough/libs/index_builds/index_build.js";

const timeFieldName = "timestamp";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

const shardKeyIndexCount = isShardedTimeseries(coll) ? 1 : 0;

assert.commandWorked(
    coll.insert({
        timestamp: ISODate(),
        a: 1,
        name: "Dallas",
        loc: {type: "Point", coordinates: [-96.808891, 32.779]},
    }),
);
assert.commandWorked(
    coll.insert({
        timestamp: ISODate(),
        a: 1,
        name: "Paris TX",
        loc: {type: "Point", coordinates: [-95.555513, 33.6609389]},
    }),
);
assert.commandWorked(
    coll.insert({
        timestamp: ISODate(),
        a: 2,
        name: "Houston",
        loc: {type: "Point", coordinates: [-95.3632715, 29.7632836]},
    }),
);
assert.commandWorked(
    coll.insert({
        timestamp: ISODate(),
        a: 1,
        name: "San Antonio",
        loc: {type: "Point", coordinates: [-98.4936282, 29.4241219]},
    }),
);
assert.commandWorked(
    coll.insert({
        timestamp: ISODate(),
        a: 3,
        name: "LA",
        loc: {type: "Point", coordinates: [-118.2436849, 34.0522342]},
    }),
);
assert.commandWorked(
    coll.insert({
        timestamp: ISODate(),
        a: 3,
        name: "Berkeley",
        loc: {type: "Point", coordinates: [-122.272747, 37.8715926]},
    }),
);
assert.commandWorked(
    coll.insert({
        timestamp: ISODate(),
        a: 1,
        name: "NYC",
        loc: {type: "Point", coordinates: [-74.0059729, 40.7142691]},
    }),
);

let southWestUSPolygon = {
    type: "Polygon",
    coordinates: [
        [
            [-97.516473, 26.02054],
            [-106.528371, 31.895644],
            [-118.646927, 33.748207],
            [-119.591751, 34.348991],
            [-103.068314, 36.426696],
            [-100.080033, 36.497382],
            [-99.975048, 34.506004],
            [-94.24019, 33.412542],
            [-94.0754, 29.72564],
            [-97.516473, 26.02054],
        ],
    ],
};

let texasPolygon = {
    type: "Polygon",
    coordinates: [
        [
            [-97.516473, 26.02054],
            [-106.528371, 31.895644],
            [-103.034724, 31.932947],
            [-103.068314, 36.426696],
            [-100.080033, 36.497382],
            [-99.975048, 34.506004],
            [-94.24019, 33.412542],
            [-94.0754, 29.72564],
            [-97.516473, 26.02054],
        ],
    ],
};
// Create index to cover Texas and Southern California.
assert.commandWorked(
    createRawTimeseriesIndex(
        coll,
        {a: 1},
        {
            partialFilterExpression: {
                $_internalBucketGeoWithin: {
                    withinRegion: {
                        $geometry: southWestUSPolygon,
                    },
                    field: "loc",
                },
            },
        },
    ),
);

IndexBuildTest.assertIndexes(
    getTimeseriesCollForRawOps(coll),
    1 + shardKeyIndexCount,
    ["a_1"],
    [] /* notReadyIndexes */,
    kRawOperationSpec,
);

let findAndExplain = assert.commandWorked(
    getTimeseriesCollForRawOps(coll)
        .find({
            a: 1,
            $_internalBucketGeoWithin: {
                withinRegion: {
                    $geometry: texasPolygon,
                },
                field: "loc",
            },
        })
        .rawData()
        .explain(),
);

assert(isIxscan(db, getWinningPlanFromExplain(findAndExplain)));

// Unlike the example above, this query provides a different argument for "field" than what we
// indexed the collection on. In this case, we cannot use our index and expect to have to do a
// collection scan.
findAndExplain = assert.commandWorked(
    getTimeseriesCollForRawOps(coll)
        .find({
            a: 1,
            $_internalBucketGeoWithin: {
                withinRegion: {
                    $geometry: texasPolygon,
                },
                field: "geoloc",
            },
        })
        .rawData()
        .explain(),
);

assert(isCollscan(db, getWinningPlanFromExplain(findAndExplain)));
assert.commandWorked(getTimeseriesCollForRawOps(coll).dropIndexes("*", kRawOperationSpec));

// Create a smaller index and query for a larger region, resulting in a collection scan.
assert.commandWorked(
    createRawTimeseriesIndex(
        coll,
        {a: 1},
        {
            partialFilterExpression: {
                $_internalBucketGeoWithin: {
                    withinRegion: {
                        $geometry: texasPolygon,
                    },
                    field: "loc",
                },
            },
        },
    ),
);
IndexBuildTest.assertIndexes(
    getTimeseriesCollForRawOps(coll),
    1 + shardKeyIndexCount,
    ["a_1"],
    [] /* notReadyIndexes */,
    kRawOperationSpec,
);

findAndExplain = assert.commandWorked(
    getTimeseriesCollForRawOps(coll)
        .find({
            a: 1,
            $_internalBucketGeoWithin: {
                withinRegion: {
                    $geometry: southWestUSPolygon,
                },
                field: "loc",
            },
        })
        .rawData()
        .explain(),
);
assert(isCollscan(db, getWinningPlanFromExplain(findAndExplain)));
