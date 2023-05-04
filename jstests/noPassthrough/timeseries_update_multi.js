/**
 * Tests running the multi:true update command on a time-series collection.
 * TODO SERVER-73454 Remove this file.
 *
 * @tags: [
 *   # Specifically testing multi-updates.
 *   requires_multi_updates,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesSupport
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

const conn = MongoRunner.runMongod({});
const testDB = conn.getDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

/**
 * Confirms that a set of updates returns the expected set of documents.
 */
function testUpdate({
    initialDocList,
    createCollectionWithMetaField = true,
    updateList,
    resultDocList,
    nMatched,
    nModified = nMatched,
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

    assert.eq(nMatched, res.n);
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

const doc_id_1_a_b_no_metrics = {
    _id: 1,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", b: "B"},
};
const doc_id_2_a_b_array_metric = {
    _id: 2,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", b: "B"},
    f: [{"k": "K", "v": "V"}],
};
const doc_id_3_a_b_string_metric = {
    _id: 3,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", b: "B"},
    f: "F",
};
const doc_id_4_no_meta_string_metric = {
    _id: 4,
    [timeFieldName]: dateTime,
    f: "F",
};
const doc_id_5_a_c_array_metric = {
    _id: 5,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", c: "C"},
    f: [2, 3],
};
const doc_id_6_a_c_array_metric = {
    _id: 6,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", c: "C"},
    f: [1, 10],
};
const doc_id_7_no_meta_int_metric = {
    _id: 7,
    [timeFieldName]: dateTime,
    g: 1,
};
const doc_id_8_array_meta = {
    _id: 8,
    [timeFieldName]: dateTime,
    [metaFieldName]: [1, 2, 3, 4]
};

/**
 * Tests op-style updates
 */
// Query on the _id field and modify the metaField.
(function testMetricFieldQueryMetaFieldUpdate() {
    testUpdate({
        initialDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
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
        nMatched: 2,
    });
})();

// Query doesn't match any docs.
(function testZeroMeasurementUpdate() {
    testUpdate({
        initialDocList:
            [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric, doc_id_5_a_c_array_metric],
        updateList: [{
            q: {someField: "doesNotExist"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList:
            [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric, doc_id_5_a_c_array_metric],
        nMatched: 0,
    });
})();

// No-op update.
(function testNoopUpdate() {
    testUpdate({
        initialDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
        updateList: [{
            q: {},
            u: {$set: {[metaFieldName]: {a: "A", b: "B"}}},
            multi: true,
        }],
        resultDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
        nMatched: 2,
        nModified: 0
    });
})();

// Query on the metaField and modify the timeField.
(function testMetaFieldQueryTimeFieldUpdate() {
    testUpdate({
        initialDocList:
            [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric, doc_id_5_a_c_array_metric],
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
            doc_id_5_a_c_array_metric
        ],
        nMatched: 2,
    });
})();

// Query on the metaField and a metric field.
(function testMetaFieldQueryMetricFieldMetric() {
    testUpdate({
        initialDocList: [doc_id_3_a_b_string_metric, doc_id_2_a_b_array_metric],
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
            doc_id_2_a_b_array_metric
        ],
        nMatched: 1,
    });
})();

// Query on the metaField and modify the metaField and a metric field.
(function testMetaFieldQueryMetaAndMetricFieldUpdate() {
    testUpdate({
        initialDocList: [doc_id_3_a_b_string_metric, doc_id_2_a_b_array_metric],
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
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: {c: "C"},
                f: "FF",
            }
        ],
        nMatched: 2,
    });
})();

// This command will fail because all time-series collections require a time field.
(function testRemoveTimeField() {
    testUpdate({
        initialDocList: [doc_id_3_a_b_string_metric, doc_id_5_a_c_array_metric],
        updateList: [{
            q: {f: "F"},
            u: {$unset: {[timeFieldName]: ""}},
            multi: true,
        }],
        resultDocList: [
            doc_id_3_a_b_string_metric,
            doc_id_5_a_c_array_metric,
        ],
        nMatched: 0,
        failCode: ErrorCodes.BadValue,
    });
})();

// Query on the time field and remove the metaField.
(function testTimeFieldQueryRemoveMetaField() {
    testUpdate({
        initialDocList:
            [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric, doc_id_5_a_c_array_metric],
        updateList: [{
            q: {[timeFieldName]: dateTime},
            u: {$unset: {[metaFieldName]: ""}},
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
        nMatched: 3,
    });
})();

(function testRenameMetaField() {
    // Rename the metaField.
    testUpdate({
        initialDocList: [doc_id_3_a_b_string_metric],
        updateList: [{
            q: {},
            u: {$rename: {[metaFieldName]: "Z"}},
            multi: true,
        }],
        resultDocList: [
            {
                _id: 3,
                [timeFieldName]: dateTime,
                Z: {a: "A", b: "B"},
                f: "F",
            },
        ],
        nMatched: 1,
    });
})();

// Rename a subfield of the metaField to something not in the metaField.
(function testRenameMetaSubfield() {
    testUpdate({
        initialDocList: [doc_id_3_a_b_string_metric],
        updateList: [{
            q: {[metaFieldName + ".a"]: "A"},
            u: {$rename: {[metaFieldName + ".a"]: "Z.a"}},
            multi: true,
        }],
        resultDocList: [
            {
                _id: 3,
                [timeFieldName]: dateTime,
                [metaFieldName]: {b: "B"},
                Z: {a: "A"},
                f: "F",
            },
        ],
        nMatched: 1,
    });
})();

// Expand a metric field.
(function testExpandMetricField() {
    testUpdate({
        initialDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
        updateList: [{
            q: {[metaFieldName]: {a: "A", b: "B"}},
            u: {$set: {f: "x".repeat(3 * 1024 * 1024)}},
            multi: true,
        }],
        resultDocList: [
            {
                _id: 1,
                [timeFieldName]: dateTime,
                [metaFieldName]: {a: "A", b: "B"},
                f: "x".repeat(3 * 1024 * 1024),
            },
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: {a: "A", b: "B"},
                f: "x".repeat(3 * 1024 * 1024),
            },
        ],
        nMatched: 2,
    });
})();

// Change the type of an existing field.
(function testChangeExistingFieldType() {
    testUpdate({
        initialDocList: [doc_id_2_a_b_array_metric, doc_id_3_a_b_string_metric],
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
        nMatched: 2,
    });
})();

// Add a new field.
(function testAddNewField() {
    testUpdate({
        initialDocList:
            [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric, doc_id_3_a_b_string_metric],
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
            doc_id_3_a_b_string_metric
        ],
        nMatched: 2,
    });
})();

(function testArrayModifier() {
    testUpdate({
        initialDocList:
            [doc_id_2_a_b_array_metric, doc_id_5_a_c_array_metric, doc_id_6_a_c_array_metric],
        updateList: [{
            q: {f: {$gt: 2}},
            u: {$set: {'f.$': 20}},
            multi: true,
        }],
        resultDocList: [
            doc_id_2_a_b_array_metric,
            {
                _id: 5,
                [timeFieldName]: dateTime,
                [metaFieldName]: {a: "A", c: "C"},
                f: [2, 20],
            },
            {
                _id: 6,
                [timeFieldName]: dateTime,
                [metaFieldName]: {a: "A", c: "C"},
                f: [1, 20],
            }
        ],
        nMatched: 2,
    });
})();

(function testMetaFieldArrayModifier() {
    testUpdate({
        initialDocList: [doc_id_8_array_meta, doc_id_2_a_b_array_metric],
        updateList: [{
            q: {[metaFieldName]: {$gt: 2}},
            u: {$set: {[metaFieldName + '.$']: 20}},
            multi: true,
        }],
        resultDocList: [
            {_id: 8, [timeFieldName]: dateTime, [metaFieldName]: [1, 2, 20, 4]},
            doc_id_2_a_b_array_metric
        ],
        nMatched: 1,
    });
})();

(function testChangeArrayElementType() {
    testUpdate({
        initialDocList:
            [doc_id_2_a_b_array_metric, doc_id_5_a_c_array_metric, doc_id_6_a_c_array_metric],
        updateList: [{
            q: {f: {$lte: 2}},
            u: {$set: {'f.$': {k: "v"}}},
            multi: true,
        }],
        resultDocList: [
            doc_id_2_a_b_array_metric,
            {
                _id: 5,
                [timeFieldName]: dateTime,
                [metaFieldName]: {a: "A", c: "C"},
                f: [{k: "v"}, 3],
            },
            {
                _id: 6,
                [timeFieldName]: dateTime,
                [metaFieldName]: {a: "A", c: "C"},
                f: [{k: "v"}, 10],
            }
        ],
        nMatched: 2,
    });
})();

(function testChangeMeasurementId() {
    testUpdate({
        initialDocList: [doc_id_1_a_b_no_metrics],
        updateList: [{
            q: {},
            u: {$set: {_id: 10}},
            multi: true,
        }],
        resultDocList: [{
            _id: 10,
            [timeFieldName]: dateTime,
            [metaFieldName]: {a: "A", b: "B"},
        }],
        nMatched: 1
    });
})();

/**
 * Tests pipeline-style updates
 */
// Add a field of the sum of an array field using aggregation pipeline.
// TODO SERVER-73143 Enable these tests.
// testUpdate({
//     initialDocList: [doc_id_5_a_c_array_metric, doc_id_6_a_c_array_metric],
//     updateList: [{
//         q: {[metaFieldName]: {a: "A", c: "C"}},
//         u: [{$set: {sumF: {$sum: "$f"}}}],
//         multi: true,
//     }],
//     resultDocList: [
//         {
//             _id: 5,
//             [timeFieldName]: dateTime,
//             [metaFieldName]: {a: "A", c: "C"},
//             f: [2, 3],
//             sumF: 5,
//         },
//         {
//             _id: 6,
//             [timeFieldName]: dateTime,
//             [metaFieldName]: {a: "A", c: "C"},
//             f: [1, 10],
//             sumF: 11,
//         },
//     ],
//     nMatched: 2,
// });

// Add a new field for all measurements.
// testUpdate({
//     initialDocList: [doc_id_4_no_meta_string_metric, doc_id_7_no_meta_int_metric],
//     createCollectionWithMetaField: false,
//     updateList: [{
//         q: {},
//         u: [{$set: {newField: true}}],
//         multi: true,
//     }],
//     resultDocList: [
//         {
//             _id: 4,
//             [timeFieldName]: dateTime,
//             f: "F",
//             newField: true,
//         },
//         {
//             _id: 7,
//             [timeFieldName]: dateTime,
//             g: 1,
//             newField: true,
//         },
//     ],
//     nMatched: 2,
// });

/**
 * Tests upsert with multi:true.
 */
// TODO SERVER-76551 Enable this test.
// testUpdate({
//     initialDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
//     updateList: [{
//         q: {[metaFieldName]: {z: "Z"}},
//         u: {$set: {[timeFieldName]: dateTime}},
//         upsert: true,
//         multi: true,
//     }],
//     resultDocList: [
//         doc_id_1_a_b_no_metrics,
//         doc_id_2_a_b_array_metric,
//         {[timeFieldName]: dateTime},
//     ],
//     nMatched: 1,
//     nModified: 0,
// });

MongoRunner.stopMongod(conn);
})();
