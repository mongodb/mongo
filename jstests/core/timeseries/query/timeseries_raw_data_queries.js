/**
 * Performs basic read operations on the buckets of a time-series collection using rawData.
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_transactions,
 *   # TODO (SERVER-104682): Remove this exclusion tag once the bug is fixed
 *   # Explain on $indexStats aggregation with rawData on mongos
 *   # does not contain queryShapeHash field.
 *   known_query_shape_computation_problem,
 * ]
 */

import {
    getTimeseriesCollForRawOps,
    kIsRawOperationSupported,
    kRawOperationSpec,
} from "jstests/core/libs/raw_operation_utils.js";

// Set up some testing buckets to work with
const timeField = "t";
const metaField = "m";
const t = new Date("2002-05-29T00:00:00Z");

const coll = db[jsTestName()];

assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));
assert.commandWorked(
    coll.insertMany([
        {[timeField]: t, [metaField]: "1", v: "replacement"},
        {[timeField]: t, [metaField]: "2", v: "baz"},
        {[timeField]: t, [metaField]: "2", v: "qux"},
    ]),
);

assert(coll.drop());

function crudTest(fn, addStartingMeasurements = true) {
    db.createCollection(coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}});
    if (addStartingMeasurements) {
        assert.commandWorked(
            coll.insertMany([
                {[timeField]: t, [metaField]: "1", v: "foo"},
                {[timeField]: t, [metaField]: "1", v: "bar"},
                {[timeField]: t, [metaField]: "2", v: "baz"},
            ]),
        );
    }
    fn();
    assert(coll.drop());
}

// aggregate()
crudTest(() => {
    const agg = getTimeseriesCollForRawOps(coll).aggregate([{$match: {"control.count": 2}}], kRawOperationSpec);
    assert.eq(agg.toArray().length, 1);

    assert.eq(
        getTimeseriesCollForRawOps(coll)
            .aggregate([{$indexStats: {}}, {$match: {name: "m_1_t_1"}}], kRawOperationSpec)
            .toArray()[0].key,
        {
            meta: 1,
            "control.min.t": 1,
            "control.max.t": 1,
        },
    );
});

// count()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll).count({"control.count": 2}, kRawOperationSpec), 1);
});

// countDocuments()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll).countDocuments({"control.count": 2}, kRawOperationSpec), 1);
});

// distinct()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll).distinct("control.count", {}, kRawOperationSpec).sort(), [1, 2]);
});

// find()
crudTest(() => {
    assert.eq(getTimeseriesCollForRawOps(coll).find().rawData().length(), 2);
    assert.eq(getTimeseriesCollForRawOps(coll).find({"control.count": 2}).rawData().length(), 1);
});

// findOne()
crudTest(() => {
    const retrievedBucket = getTimeseriesCollForRawOps(coll).findOne(
        {"control.count": 2},
        null,
        null,
        null,
        null,
        kIsRawOperationSupported /* rawData */,
    );
    assert.eq(retrievedBucket.control.count, 2);
    assert.eq(retrievedBucket.meta, "1");
});
