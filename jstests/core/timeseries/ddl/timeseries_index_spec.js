/**
 * Tests that the original user index definition is stored on the raw transformed index definitions
 * for newly supported index types introduced in v6.0. Raw indexes created directly over the buckets
 * do not have an original user index definition and rely on the reverse mapping mechanism.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # During fcv upgrade/downgrade the index created might not be what we expect.
 * ]
 */
import {
    createRawTimeseriesIndex,
    getTimeseriesCollForRawOps,
    kRawOperationSpec
} from "jstests/core/libs/raw_operation_utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";
import {isShardedTimeseries} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

TimeseriesTest.run(() => {
    const collName = jsTestName();

    const timeFieldName = "tm";
    const metaFieldName = "mm";

    const coll = db.getCollection(collName);
    coll.drop();

    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    // If the collection is sharded, we expect an implicitly-created index on time. This index will
    // be the same index as the result of createIndex({timeField: 1}). Therefore we cannot create
    // nor drop an identical index with a different name.
    if (!isShardedTimeseries(coll)) {
        assert.commandWorked(
            coll.createIndex({[timeFieldName]: 1}, {name: "timefield_downgradable"}));
        TimeseriesTest.verifyAndDropIndex(
            coll, /*shouldHaveOriginalSpec=*/ false, "timefield_downgradable");
    }

    assert.commandWorked(coll.createIndex({[metaFieldName]: 1}, {name: "metafield_downgradable"}));
    TimeseriesTest.verifyAndDropIndex(
        coll, /*shouldHaveOriginalSpec=*/ false, "metafield_downgradable");

    assert.commandWorked(coll.createIndex({[timeFieldName]: 1, [metaFieldName]: 1},
                                          {name: "time_meta_field_downgradable"}));
    TimeseriesTest.verifyAndDropIndex(
        coll, /*shouldHaveOriginalSpec=*/ false, "time_meta_field_downgradable");

    assert.commandWorked(coll.createIndex({x: 1}, {name: "x_1"}));
    TimeseriesTest.verifyAndDropIndex(coll, /*shouldHaveOriginalSpec=*/ true, "x_1");

    assert.commandWorked(
        coll.createIndex({x: 1}, {name: "x_partial", partialFilterExpression: {x: {$gt: 5}}}));
    TimeseriesTest.verifyAndDropIndex(coll, /*shouldHaveOriginalSpec=*/ true, "x_partial");

    assert.commandWorked(coll.createIndex(
        {[timeFieldName]: 1}, {name: "time_partial", partialFilterExpression: {x: {$gt: 5}}}));
    TimeseriesTest.verifyAndDropIndex(coll, /*shouldHaveOriginalSpec=*/ true, "time_partial");

    assert.commandWorked(coll.createIndex(
        {[metaFieldName]: 1}, {name: "meta_partial", partialFilterExpression: {x: {$gt: 5}}}));
    TimeseriesTest.verifyAndDropIndex(coll, /*shouldHaveOriginalSpec=*/ true, "meta_partial");

    assert.commandWorked(
        coll.createIndex({[metaFieldName]: 1, x: 1},
                         {name: "meta_x_partial", partialFilterExpression: {x: {$gt: 5}}}));
    TimeseriesTest.verifyAndDropIndex(coll, /*shouldHaveOriginalSpec=*/ true, "meta_x_partial");

    // Creating a raw index directly over the bucket documents is permitted. However, these types
    // of index creations will not have an "originalSpec" field and rely on the reverse mapping
    // mechanism.
    assert.commandWorked(
        createRawTimeseriesIndex(coll, {"control.min.y": 1, "control.max.y": 1}, {name: "y"}));

    let foundIndex = false;
    let bucketIndexes = getTimeseriesCollForRawOps(coll).getIndexes(kRawOperationSpec);
    for (const index of bucketIndexes) {
        if (index.name == "y") {
            foundIndex = true;
            assert(!index.hasOwnProperty("originalSpec"));
            break;
        }
    }
    assert(foundIndex);

    // Verify that the bucket index can map to a user index.
    foundIndex = false;
    let userIndexes = coll.getIndexes();
    for (const index of userIndexes) {
        if (index.name == "y") {
            foundIndex = true;
            assert(!index.hasOwnProperty("originalSpec"));
            assert.eq(index.key, {y: 1});
            break;
        }
    }
    assert(foundIndex);
});
