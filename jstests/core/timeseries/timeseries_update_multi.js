/**
 * Tests running the multi:true update command on a time-series collection.
 *
 * @tags: [
 *   # Specifically testing multi-updates.
 *   requires_multi_updates,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport,
 *   # TODO (SERVER-73454): Re-enable the tests.
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
"use strict";

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const dateTimeUpdated = ISODate("2023-01-27T16:00:00Z");
const collNamePrefix = "timeseries_update_multi_";
let count = 0;

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

/**
 * Confirms that a set of updates returns the expected set of documents.
 */
function testUpdate({
    initialDocList,
    createCollectionWithMetaField = true,
    updateList,
    resultDocList,
    n,
    nModified = n,
    failCode,
}) {
    const coll = testDB.getCollection(collNamePrefix + count++);
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: createCollectionWithMetaField
            ? {timeField: timeFieldName, metaField: metaFieldName}
            : {timeField: timeFieldName},
    }));

    assert.commandWorked(coll.insert(initialDocList));

    const updateCommand = {update: coll.getName(), updates: updateList};
    const res = failCode ? assert.commandFailedWithCode(testDB.runCommand(updateCommand), failCode)
                         : assert.commandWorked(testDB.runCommand(updateCommand));

    assert.eq(n, res.n);
    assert.eq(nModified, res.nModified);
    const resDocs = coll.find().toArray();
    assert.eq(resDocs.length, resultDocList.length);

    resultDocList.forEach(resultDoc => {
        assert.docEq(resultDoc,
                     coll.findOne({_id: resultDoc._id}),
                     "Expected document " + resultDoc["_id"] +
                         " not found in result collection:" + tojson(resDocs));
    });
}

const doc_a_b_no_metrics = {
    _id: 1,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", b: "B"},
};
const doc_a_b_array_metric = {
    _id: 2,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", b: "B"},
    f: [{"k": "K", "v": "V"}],
};
const doc_a_b_string_metric = {
    _id: 3,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", b: "B"},
    f: "F",
};
const doc_no_meta_string_metric = {
    _id: 4,
    [timeFieldName]: dateTime,
    f: "F",
};
const doc_a_c_array_metric_1 = {
    _id: 5,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", c: "C"},
    f: [2, 3],
};
const doc_a_c_array_metric_2 = {
    _id: 6,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", c: "C"},
    f: [1, 10],
};
const doc_no_meta_int_metric = {
    _id: 7,
    [timeFieldName]: dateTime,
    g: 1,
};

/**
 * Tests op-style updates
 */
// Query on the _id field and modify the metaField.
testUpdate({
    initialDocList: [doc_a_b_no_metrics, doc_a_b_array_metric],
    updateList: [{
        q: {_id: {$lt: 10}},
        u: {$set: {[metaFieldName]: {c: "C"}}},
        multi: true,
    }],
    resultDocList: [
        {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "C"}},
        {
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C"},
            f: [{"k": "K", "v": "V"}],
        },
    ],
    n: 2,
});

// Query on the metaField and modify the timeField.
testUpdate({
    initialDocList: [doc_a_b_no_metrics, doc_a_b_array_metric],
    updateList: [{
        q: {[metaFieldName]: {a: "A", b: "B"}},
        u: {$set: {[timeFieldName]: dateTimeUpdated}},
        multi: true,
    }],
    resultDocList: [
        {
            _id: 1,
            [timeFieldName]: dateTimeUpdated,
            [metaFieldName]: {a: "A", b: "B"},
        },
        {
            _id: 2,
            [timeFieldName]: dateTimeUpdated,
            [metaFieldName]: {a: "A", b: "B"},
            f: [{"k": "K", "v": "V"}],
        },
    ],
    n: 2,
});

// Query on the metaField and a field that is not the metaField.
testUpdate({
    initialDocList: [doc_a_b_string_metric],
    updateList: [{
        q: {[metaFieldName]: {a: "A", b: "B"}, f: "F"},
        u: {$set: {[metaFieldName]: {c: "C"}}},
        multi: true,
    }],
    resultDocList: [
        {
            _id: 3,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C"},
            f: "F",
        },
    ],
    n: 1,
});

// Query on the metaField and modify the metaField and a field that is not the metaField.
testUpdate({
    initialDocList: [doc_a_b_string_metric],
    updateList: [{
        q: {[metaFieldName]: {a: "A", b: "B"}},
        u: {$set: {[metaFieldName]: {c: "C"}, f: "FF"}},
        multi: true,
    }],
    resultDocList: [
        {
            _id: 3,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C"},
            f: "FF",
        },
    ],
    n: 1,
});

// This command will fail because all time-series collections require a time field.
testUpdate({
    initialDocList: [doc_a_b_string_metric, doc_a_c_array_metric_1],
    updateList: [{
        q: {f: "F"},
        u: {$unset: {[timeFieldName]: ""}},
        multi: true,
    }],
    resultDocList: [
        doc_a_b_string_metric,
        doc_a_c_array_metric_1,
    ],
    n: 0,
    failCode: ErrorCodes.InvalidOptions,
});

// Query on the time field and remove the metaField.
testUpdate({
    initialDocList: [doc_a_b_no_metrics, doc_a_b_array_metric, doc_a_c_array_metric_1],
    updateList: [{
        q: {[timeField]: dateTime},
        u: {$unset: {[metaFieldName]: ""}, multi: true},
        multi: true,
    }],
    resultDocList: [
        {
            _id: 1,
            [timeFieldName]: dateTime,
        },
        {
            _id: 2,
            [timeFieldName]: dateTime,
            f: [{"k": "K", "v": "V"}],
        },
        {
            _id: 5,
            [timeFieldName]: dateTime,
            f: [2, 3],
        },
    ],
    n: 3,
});

// Expand a metric field.
testUpdate({
    initialDocList: [doc_a_b_no_metrics, doc_a_b_array_metric],
    updateList: [{
        q: {[metaFieldName]: {a: "A", b: "B"}},
        u: {$set: {f: "x".repeat(5 * 1024 * 1024)}},
        multi: true,
    }],
    resultDocList: [
        {
            _id: 1,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", b: "B"},
            f: "x".repeat(5 * 1024 * 1024),
        },
        {
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", b: "B"},
            f: "x".repeat(5 * 1024 * 1024),
        },
    ],
    n: 2,
});

// Change the type of an existing field.
testUpdate({
    initialDocList: [doc_a_b_array_metric, doc_a_b_string_metric],
    updateList: [{
        q: {[metaFieldName]: {a: "A", b: "B"}},
        u: {$set: {f: "X"}},
        multi: true,
    }],
    resultDocList: [
        {
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", b: "B"},
            f: "X",
        },
        {
            _id: 3,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", b: "B"},
            f: "X",
        },
    ],
    n: 2,
});

// Add a new field.
testUpdate({
    initialDocList: [doc_a_b_no_metrics, doc_a_b_array_metric, doc_a_b_string_metric],
    updateList: [{
        q: {_id: {$lt: 3}},
        u: {$set: {g: 42}},
        multi: true,
    }],
    resultDocList: [
        {
            _id: 1,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", b: "B"},
            g: 42,
        },
        {
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", b: "B"},
            f: [{"k": "K", "v": "V"}],
            g: 42,
        },
        doc_a_b_string_metric
    ],
    n: 2,
});

/**
 * Tests pipeline-style updates
 */
// Add a field of the sum of an array field using aggregation pipeline.
testUpdate({
    initialDocList: [doc_a_c_array_metric_1, doc_a_c_array_metric_2],
    updateList: [{
        q: {[metaFieldName]: {a: "A", c: "C"}},
        u: [{$set: {sumF: {$sum: "$f"}}}],
        multi: true,
    }],
    resultDocList: [
        {
            _id: 5,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", c: "C"},
            f: [2, 3],
            sumF: 5,
        },
        {
            _id: 6,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", c: "C"},
            f: [5, 6],
            sumF: 11,
        },
    ],
    n: 2,
});

// Add a new field for all measurements.
testUpdate({
    initialDocList: [doc_no_meta_string_metric, doc_no_meta_int_metric],
    createCollectionWithMetaField: false,
    updateList: [{
        q: {},
        u: [{$set: {newField: true}}],
        multi: true,
    }],
    resultDocList: [
        {
            _id: 4,
            [timeFieldName]: dateTime,
            f: "F",
            newField: true,
        },
        {
            _id: 7,
            [timeFieldName]: dateTime,
            g: 1,
            newField: true,
        },
    ],
    n: 2,
});

/**
 * Tests upsert with multi:true.
 */
testUpdate({
    initialDocList: [doc_a_b_no_metrics, doc_a_b_array_metric],
    updateList: [{
        q: {[metaFieldName]: {z: "Z"}},
        u: {$set: {[timeFieldName]: dateTime}},
        upsert: true,
        multi: true,
    }],
    resultDocList: [
        doc_a_b_no_metrics,
        doc_a_b_array_metric,
        {[timeFieldName]: dateTime},
    ],
    n: 1,
    nModified: 0,
});
})();
