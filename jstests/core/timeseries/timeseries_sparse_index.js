/**
 * Tests that sparse indexes are not allowed on a time-series measurement field.
 *
 * @tags: [
 *   # This test makes assertions on listIndexes and on the order of the indexes returned.
 *   assumes_no_implicit_index_creation,
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const collName = "timeseries_sparse_index";

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    // Unique metadata values to create separate buckets.
    const docs = [
        {
            _id: 0,
            [timeFieldName]: ISODate(),
            [metaFieldName]: {tag: "a", loc: {type: "Point", coordinates: [3, 3]}},
            x: 1
        },
        {_id: 1, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "b"}, y: 1},
        {_id: 2, [timeFieldName]: ISODate(), [metaFieldName]: {tag: "c"}, x: 1, y: 1}
    ];

    const setup = function(keyForCreate, shouldSucceed) {
        const coll = db.getCollection(collName);
        const bucketsColl = db.getCollection("system.buckets." + collName);
        coll.drop();

        const options = {sparse: true};
        jsTestLog("Setting up collection: " + coll.getFullName() +
                  " with index: " + tojson(keyForCreate) + " and options: " + tojson(options));

        assert.commandWorked(db.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

        const numUserIndexesBefore = coll.getIndexes().length;
        const numBucketIndexesBefore = bucketsColl.getIndexes().length;

        // Insert data on the time-series collection and index it.
        assert.commandWorked(insert(coll, docs), "failed to insert docs: " + tojson(docs));

        const res = coll.createIndex(keyForCreate, options);
        if (shouldSucceed) {
            assert.commandWorked(res,
                                 "failed to create index: " + tojson(keyForCreate) +
                                     " with options: " + tojson(options));

            assert.eq(numUserIndexesBefore + 1, coll.getIndexes().length);
            assert.eq(numBucketIndexesBefore + 1, bucketsColl.getIndexes().length);
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
        }
    };

    const testIndex = (userKeyPattern, bucketsKeyPattern, numDocs) => {
        const coll = db.getCollection(collName);
        setup(userKeyPattern, /*shouldSucceed=*/ true);
        TimeseriesTest.checkIndex(coll, userKeyPattern, bucketsKeyPattern, numDocs);
        assert.commandWorked(coll.dropIndex(userKeyPattern));
    };

    // Test metadata-only sparse indexes.
    testIndex({[`${metaFieldName}.tag`]: 1, [`${metaFieldName}.loc`]: "2dsphere"},
              {"meta.tag": 1, "meta.loc": "2dsphere"},
              1);
    testIndex({[`${metaFieldName}.tag`]: 1}, {"meta.tag": 1}, 3);
    testIndex({[`${metaFieldName}.abc`]: 1}, {"meta.abc": 1}, 0);

    // Cannot create sparse indexes on time-series measurements.
    setup({x: 1}, /*shouldSucceed=*/ false);
    setup({y: -1}, /*shouldSucceed=*/ false);
    setup({x: 1, y: 1}, /*shouldSucceed=*/ false);
    setup({z: 1}, /*shouldSucceed=*/ false);

    // Compound sparse indexes are not allowed if measurements are involved.
    setup({x: 1, [`${metaFieldName}.loc`]: "2dsphere"}, /*shouldSucceed=*/ false);
    setup({[`${timeFieldName}`]: 1, x: 1}, /*shouldSucceed=*/ false);

    // Test compound time and metadata sparse indexes.
    testIndex({[`${timeFieldName}`]: 1, [`${metaFieldName}.tag`]: 1},
              {"control.min.tm": 1, "control.max.tm": 1, "meta.tag": 1},
              3);
    testIndex({[`${metaFieldName}.abc`]: 1, [`${timeFieldName}`]: -1},
              {"meta.abc": 1, "control.max.tm": -1, "control.min.tm": -1},
              3);
});
