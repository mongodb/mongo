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

const collName = jsTestName();

const timeFieldName = "tm";
const metaFieldName = "mm";

const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(db.createCollection(
    coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

{
    // If the collection is sharded, we expect an implicitly-created index on time. This index will
    // be the same index as the result of createIndex({timeField: 1}). Therefore we cannot create
    // nor drop an identical index with a different name.
    if (!isShardedTimeseries(coll)) {
        assert.commandWorked(
            coll.createIndex({[timeFieldName]: 1}, {name: "timefield_downgradable"}));
        TimeseriesTest.verifyAndDropIndex(
            coll, /*shouldHaveOriginalSpec=*/ false, "timefield_downgradable");
    }
}

function testIndexSpec(indexSpec) {
    jsTest.log(`Testing index: ${tojsononeline(indexSpec)}`);

    assert.commandWorked(coll.createIndex(indexSpec.key, indexSpec.options));
    const indexEntry = coll.getIndexByName(indexSpec.options.name);
    assert.eq(indexSpec.key, indexEntry.key);

    // The generated entry has all the options specified at creation
    for (const [key, value] of Object.entries(indexSpec.options)) {
        assert.hasFields(indexEntry, [key]);
        assert.docEq(indexEntry[key],
                     value,
                     `unexpected value for '${key}' index property. Full index entry: ${
                         tojson(indexEntry)}`);
    }
    TimeseriesTest.verifyAndDropIndex(
        coll, indexSpec.shouldHaveOriginalSpec, indexSpec.options.name);
}

const indexSpecs = [
    {
        key: {[metaFieldName]: 1},
        options: {name: "metafield_downgradable"},
        shouldHaveOriginalSpec: false,
    },
    {
        key: {[timeFieldName]: 1, [metaFieldName]: 1},
        options: {name: "time_meta_field_downgradable"},
        shouldHaveOriginalSpec: false,
    },
    {
        key: {x: 1},
        options: {name: "x_1"},
        shouldHaveOriginalSpec: true,
    },
    {
        key: {x: 1},
        options: {name: "x_partial", partialFilterExpression: {x: {$gt: 5}}},
        shouldHaveOriginalSpec: true,
    },
    {
        key: {[timeFieldName]: 1},
        options: {name: "time_partial", partialFilterExpression: {x: {$gt: 5}}},
        shouldHaveOriginalSpec: true,
    },
    {
        key: {[metaFieldName]: 1},
        options: {name: "time_partial", partialFilterExpression: {x: {$gt: 5}}},
        shouldHaveOriginalSpec: true,
    },
    {
        key: {[metaFieldName]: 1, x: 1},
        options: {name: "meta_x_partial", partialFilterExpression: {x: {$gt: 5}}},
        shouldHaveOriginalSpec: true,
    },
];

for (const indexSpec of indexSpecs) {
    testIndexSpec(indexSpec);
}

{
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
}
