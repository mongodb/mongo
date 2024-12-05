/**
 * Tests running the multi:true update command on a time-series collection.
 *
 * @tags: [
 *   # Specifically testing multi-updates.
 *   requires_multi_updates,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_non_retryable_writes,
 *   featureFlagTimeseriesUpdatesSupport,
 *   requires_fcv_80,
 * ]
 */

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const dateTimeUpdated = ISODate("2023-01-27T16:00:00Z");
const bucketParam = 3600;
const collNamePrefix = "timeseries_update_multi_";
let count = 0;

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

/**
 * Confirms that a set of updates returns the expected set of documents.
 *
 * If this is an upsert and we expect a document to be inserted, 'upsertedDoc' must be non-null. We
 * will use the 'upsertedId' returned from the update command unioned with 'upsertedDoc' to
 * construct the inserted document. This will be added to 'resultDocList' to validate the
 * collection's contents.
 */
function testUpdate({
    timeseriesOptions,
    initialDocList,
    updateList,
    resultDocList,
    nMatched,
    nModified = nMatched,
    upsertedDoc,
    failCode,
}) {
    const coll = testDB.getCollection(collNamePrefix + count++);
    assert.commandWorked(testDB.createCollection(coll.getName(), {
        timeseries: {timeField: timeFieldName, ...timeseriesOptions},
    }));

    assert.commandWorked(coll.insert(initialDocList));

    const updateCommand = {update: coll.getName(), updates: updateList};
    const res = failCode ? assert.commandFailedWithCode(coll.runCommand(updateCommand), failCode)
                         : assert.commandWorked(coll.runCommand(updateCommand));

    if (!failCode) {
        if (upsertedDoc) {
            assert.eq(1, res.n, tojson(res));
            assert.eq(0, res.nModified, tojson(res));
            assert(res.hasOwnProperty("upserted"), tojson(res));
            assert.eq(1, res.upserted.length);

            if (upsertedDoc.hasOwnProperty("_id")) {
                assert.eq(upsertedDoc._id, res.upserted[0]._id);
            } else {
                upsertedDoc["_id"] = res.upserted[0]._id;
            }
            resultDocList.push(upsertedDoc);
        } else {
            assert.eq(nMatched, res.n);
            assert.eq(nModified, res.nModified);
            assert(!res.hasOwnProperty("upserted"), tojson(res));
        }
    }

    const resDocs = coll.find().toArray();
    assert.eq(resDocs.length, resultDocList.length);

    assert.sameMembers(
        resultDocList, resDocs, "Collection contents did not match expected after update");
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
    [metaFieldName]: [1, 2, 3, 4],
    f: [4, 3, 2, 1],
};
const times = [
    dateTime,
    new Date(dateTime.getTime() - (bucketParam * 1000) / 2),
    new Date(dateTime.getTime() + (bucketParam * 1000) / 2),
    new Date(dateTime.getTime() + (bucketParam * 1000)),
];
const doc_id_9_time_string_metric = {
    _id: 9,
    [timeFieldName]: times[0],
    [metaFieldName]: {a: "A", c: "C"},
    f: "hello",
};
const doc_id_10_time_array_metric = {
    _id: 11,
    [timeFieldName]: times[1],
    [metaFieldName]: {a: "A", c: "C"},
    f: [10, 11, 12]
};
const doc_id_11_time_no_metric = {
    _id: 11,
    [timeFieldName]: times[2],
    [metaFieldName]: {a: "A", c: "C"},
};
const doc_id_12_time_int_metric = {
    _id: 12,
    [timeFieldName]: times[3],
    [metaFieldName]: {a: "A", c: "C"},
    f: 2,
};

/**
 * Tests op-style updates
 */
// Query on the _id field and modify the metaField.
(function testMetricFieldQueryMetaFieldUpdate() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
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
        timeseriesOptions: {metaField: metaFieldName},
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
        timeseriesOptions: {metaField: metaFieldName},
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
// Skip tests changing the shard key value in sharding.
if (!db.getMongo().isMongos() && !TestData.testingReplicaSetEndpoint) {
    (function testMetaFieldQueryTimeFieldUpdate() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
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
}

// Query on the metaField and a metric field.
(function testMetaFieldQueryMetricFieldMetric() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
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

// Query on the metaField and a metric field. This should be a no-op using the default collation.
(function testMetaFieldQueryMetricFieldMetricWithDefaultCollation() {
    testUpdate({
        initialDocList: [doc_id_3_a_b_string_metric, doc_id_2_a_b_array_metric],
        updateList: [{
            q: {[metaFieldName]: {a: "A", b: "B"}, f: "f"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [doc_id_3_a_b_string_metric, doc_id_2_a_b_array_metric],
        nMatched: 0,
    });
})();

// Query on the metaField and a metric field using a case insensitive collation.
(function testMetaFieldQueryMetricFieldMetricWithCaseInsensitiveCollation() {
    testUpdate({
        initialDocList: [doc_id_3_a_b_string_metric, doc_id_2_a_b_array_metric],
        updateList: [{
            q: {[metaFieldName]: {a: "A", b: "B"}, f: "f"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
            collation: {locale: "en", strength: 2}
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
        timeseriesOptions: {metaField: metaFieldName},
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
// Skip tests changing the shard key value in sharding.
if (!db.getMongo().isMongos() && !TestData.testingReplicaSetEndpoint) {
    (function testRemoveTimeField() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
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
}

// This command will fail because the time field must be a timestamp.
// Skip tests changing the shard key value in sharding.
if (!db.getMongo().isMongos() && !TestData.testingReplicaSetEndpoint) {
    (function testChangeTimeFieldType() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_3_a_b_string_metric, doc_id_5_a_c_array_metric],
            updateList: [{
                q: {f: "F"},
                u: {$set: {[timeFieldName]: "hello"}},
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
}

// Query on the time field and remove the metaField.
(function testTimeFieldQueryRemoveMetaField() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
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
        timeseriesOptions: {metaField: metaFieldName},
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
        timeseriesOptions: {metaField: metaFieldName},
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
        timeseriesOptions: {metaField: metaFieldName},
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
        timeseriesOptions: {metaField: metaFieldName},
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
        timeseriesOptions: {metaField: metaFieldName},
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

// Update a metric field with a positional operator.
(function testArrayModifier() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
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

// Update the meta field with a positional operator.
(function testMetaFieldArrayModifier() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
        initialDocList: [doc_id_8_array_meta, doc_id_2_a_b_array_metric],
        updateList: [{
            q: {[metaFieldName]: {$gt: 2}},
            u: {$set: {[metaFieldName + '.$']: 20, f: 10}},
            multi: true,
        }],
        resultDocList: [
            {_id: 8, [timeFieldName]: dateTime, [metaFieldName]: [1, 2, 20, 4], f: 10},
            doc_id_2_a_b_array_metric
        ],
        nMatched: 1,
    });
})();

// Update meta and metric fields with a positional operator.
(function testMetaAndMetricFieldArrayModifier() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
        initialDocList: [doc_id_8_array_meta, doc_id_2_a_b_array_metric],
        updateList: [{
            q: {[metaFieldName]: {$gt: 2}, f: {$gt: 2}},
            u: {$set: {[metaFieldName + '.$']: 20, 'f.$': 10}},
            multi: true,
        }],
        resultDocList: [
            {_id: 8, [timeFieldName]: dateTime, [metaFieldName]: [1, 2, 20, 4], f: [4, 3, 10, 1]},
            doc_id_2_a_b_array_metric
        ],
        nMatched: 1,
    });
})();

// Empty query and update a metric field using a positional operator.
(function testArrayModifierNoFilter() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
        initialDocList: [doc_id_3_a_b_string_metric, doc_id_5_a_c_array_metric],
        updateList: [{
            q: {},
            u: {$set: {'f.$': 20}},
            multi: true,
        }],
        resultDocList: [doc_id_3_a_b_string_metric, doc_id_5_a_c_array_metric],
        failCode: ErrorCodes.BadValue,
    });
})();

// Query on the meta field and update a metric field using a positional operator.
(function testArrayModifierMetaFilter() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
        initialDocList: [doc_id_3_a_b_string_metric, doc_id_5_a_c_array_metric],
        updateList: [{
            q: {[metaFieldName]: {a: "A", c: "C"}},
            u: {$set: {'f.$': 20}},
            multi: true,
        }],
        resultDocList: [doc_id_3_a_b_string_metric, doc_id_5_a_c_array_metric],
        failCode: ErrorCodes.BadValue,
    });
})();

(function testChangeArrayElementType() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
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
        timeseriesOptions: {metaField: metaFieldName},
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

// Use a non-idempotent update to insert the updated measurement later in the index to verify
// handling of the Halloween Problem.
(function testHalloweenProblem() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
        initialDocList: [doc_id_2_a_b_array_metric, doc_id_3_a_b_string_metric],
        updateList: [{
            q: {},
            u: {$set: {[metaFieldName + '.a']: "B"}, $inc: {x: 1}},
            multi: true,
        }],
        resultDocList: [
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: {a: "B", b: "B"},
                f: [{"k": "K", "v": "V"}],
                x: 1,
            },
            {
                _id: 3,
                [timeFieldName]: dateTime,
                [metaFieldName]: {a: "B", b: "B"},
                f: "F",
                x: 1,
            },
        ],
        nMatched: 2,
    });
})();

/**
 * Tests pipeline-style updates
 */
// Add a field of the sum of an array field using aggregation pipeline.
(function testUpdatePipelineArrayAggregation() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
        initialDocList: [doc_id_5_a_c_array_metric, doc_id_6_a_c_array_metric],
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
                f: [1, 10],
                sumF: 11,
            },
        ],
        nMatched: 2,
    });
})();

// Add a new field for all measurements.
(function testUpdatePipelineAddNewField() {
    testUpdate({
        initialDocList: [doc_id_4_no_meta_string_metric, doc_id_7_no_meta_int_metric],
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
        nMatched: 2,
    });
})();

// Cause a bucket to be split into multiple new buckets by an update, i.e. update documents in the
// same bucket to belong in different buckets.
(function testSplitBucketWithUpdate() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
        initialDocList:
            [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric, doc_id_3_a_b_string_metric],
        updateList: [{
            q: {},
            u: [{$set: {[metaFieldName]: "$f"}}],
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
                [metaFieldName]: [{"k": "K", "v": "V"}],
                f: [{"k": "K", "v": "V"}],
            },
            {
                _id: 3,
                [timeFieldName]: dateTime,
                [metaFieldName]: "F",
                f: "F",
            }
        ],
        nMatched: 3,
    });
})();

// Only touch the meta field in a pipeline update.
(function testUpdatePipelineOnlyTouchMetaField() {
    testUpdate({
        timeseriesOptions: {metaField: metaFieldName},
        initialDocList: [doc_id_1_a_b_no_metrics, doc_id_6_a_c_array_metric],
        updateList: [{
            q: {[metaFieldName]: {a: "A", b: "B"}},
            u: [{$set: {[metaFieldName]: "$" + metaFieldName + ".a"}}],
            multi: true,
        }],
        resultDocList:
            [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: "A"}, doc_id_6_a_c_array_metric],
        nMatched: 1,
    });
})();

/**
 * Tests upsert with multi:true.
 */
// Run an upsert that doesn't include an _id.
// Skip upsert tests in sharding as the query has to be on the shard key field.
if (!db.getMongo().isMongos() && !TestData.testingReplicaSetEndpoint) {
    (function testUpsertWithNoId() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
            updateList: [{
                q: {[metaFieldName]: {z: "Z"}},
                u: {$set: {[timeFieldName]: dateTime}},
                upsert: true,
                multi: true,
            }],
            resultDocList: [
                doc_id_1_a_b_no_metrics,
                doc_id_2_a_b_array_metric,
            ],
            upsertedDoc: {[metaFieldName]: {z: "Z"}, [timeFieldName]: dateTime},
        });
    })();
    // Run an upsert that includes an _id.
    (function testUpsertWithId() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateList: [{
                q: {_id: 100},
                u: {$set: {[timeFieldName]: dateTime}},
                upsert: true,
                multi: true,
            }],
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
            upsertedDoc: {_id: 100, [timeFieldName]: dateTime},
        });
    })();

    // Run an upsert that updates documents and skips the upsert.
    (function testUpsertUpdatesDocs() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
            updateList: [{
                q: {[metaFieldName + ".a"]: "A"},
                u: {$set: {f: 10}},
                upsert: true,
                multi: true,
            }],
            resultDocList: [
                {
                    _id: 1,
                    [timeFieldName]: dateTime,
                    [metaFieldName]: {a: "A", b: "B"},
                    f: 10,
                },
                {
                    _id: 2,
                    [timeFieldName]: dateTime,
                    [metaFieldName]: {a: "A", b: "B"},
                    f: 10,
                }
            ],
            nMatched: 2,
        });
    })();

    // Run an upsert that matches documents with no-op updates and skips the upsert.
    (function testUpsertMatchesDocs() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
            updateList: [{
                q: {[metaFieldName + ".a"]: "A"},
                u: {$set: {[timeFieldName]: dateTime}},
                upsert: true,
                multi: true,
            }],
            resultDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
            nMatched: 2,
            nModified: 0,
        });
    })();

    // Run an upsert that matches a bucket but no documents in it, and inserts the document into a
    // bucket with the same parameters.
    (function testUpsertIntoMatchedBucket() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
            updateList: [{
                q: {[metaFieldName]: {a: "A", b: "B"}, f: 111},
                u: {$set: {[timeFieldName]: dateTime}},
                upsert: true,
                multi: true,
            }],
            upsertedDoc: {[metaFieldName]: {a: "A", b: "B"}, [timeFieldName]: dateTime, f: 111},
            resultDocList: [doc_id_1_a_b_no_metrics, doc_id_2_a_b_array_metric],
        });
    })();

    // Run an upsert that doesn't insert a time field.
    (function testUpsertNoTimeField() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateList: [{
                q: {[metaFieldName]: {z: "Z"}},
                u: {$set: {f: 10}},
                upsert: true,
                multi: true,
            }],
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
            failCode: ErrorCodes.BadValue,
        });
    })();

    // Run an upsert where the time field is provided in the query.
    (function testUpsertQueryOnTimeField() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateList: [{
                q: {[timeFieldName]: dateTimeUpdated},
                u: {$set: {f: 10}},
                upsert: true,
                multi: true,
            }],
            upsertedDoc: {
                [timeFieldName]: dateTimeUpdated,
                f: 10,
            },
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
        });
    })();

    // Run an upsert where a document to insert is supplied by the request.
    (function testUpsertSupplyDoc() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateList: [{
                q: {[timeFieldName]: dateTimeUpdated},
                u: [{$set: {f: 10}}],
                upsert: true,
                multi: true,
                upsertSupplied: true,
                c: {new: {[timeFieldName]: dateTime, f: 100}}
            }],
            upsertedDoc: {
                [timeFieldName]: dateTime,
                f: 100,
            },
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
        });
    })();

    // Run an upsert where a document to insert is supplied by the request and does not have a time
    // field.
    (function testUpsertSupplyDocNoTimeField() {
        testUpdate({
            timeseriesOptions: {metaField: metaFieldName},
            initialDocList: [doc_id_1_a_b_no_metrics],
            updateList: [{
                q: {[timeFieldName]: dateTimeUpdated},
                u: [{$set: {f: 10}}],
                upsert: true,
                multi: true,
                upsertSupplied: true,
                c: {new: {[metaFieldName]: {a: "A"}, f: 100}}
            }],
            resultDocList: [
                doc_id_1_a_b_no_metrics,
            ],
            failCode: ErrorCodes.BadValue,
        });
    })();
}

/**
 * Tests fixed buckets optimization, which removes the residualFilter for predicates on the
 * timeField that align with the bucket boundaries.
 */

// Since the predicate is $lt and the predicate aligns with the bucket boundaries, we expect no
// residual filter. Run an upsert where a document to insert is supplied by the request.
const fixedBucketOptions = {
    metaField: metaFieldName,
    bucketMaxSpanSeconds: bucketParam,
    bucketRoundingSeconds: bucketParam
};
if (!db.getMongo().isMongos() && !TestData.testingReplicaSetEndpoint) {
    (function testUpsertSupplyDoc_NoFilter() {
        testUpdate({
            timeseriesOptions: fixedBucketOptions,
            initialDocList: [doc_id_9_time_string_metric],
            updateList: [{
                q: {[timeFieldName]: {$lt: dateTime}},
                u: [{$set: {f: 10}}],
                upsert: true,
                multi: true,
                upsertSupplied: true,
                c: {new: {[timeFieldName]: dateTime, [metaFieldName]: {"a": 100}, f: 100}}
            }],
            upsertedDoc: {
                [timeFieldName]: dateTime,
                [metaFieldName]: {"a": 100},
                f: 100,
            },
            resultDocList: [
                doc_id_9_time_string_metric,
            ],
        });
    })();
}

// Since the predicate is $gt we expect a residual filter. We will only touch the meta field in
// a pipeline update.
(function testPipelineMetaField_WithFilter() {
    testUpdate({
        timeseriesOptions: fixedBucketOptions,
        initialDocList:
            [doc_id_9_time_string_metric, doc_id_11_time_no_metric, doc_id_12_time_int_metric],
        updateList: [{
            q: {[timeFieldName]: {$gt: dateTime}},
            u: [{$set: {[metaFieldName]: "$" + metaFieldName + ".a"}}],
            multi: true,
        }],
        resultDocList: [
            doc_id_9_time_string_metric,
            {_id: 11, [timeFieldName]: times[2], [metaFieldName]: "A"},
            {_id: 12, [timeFieldName]: times[3], [metaFieldName]: "A", f: 2},
        ],
        nMatched: 2,
    });
})();

// Since the predicate is a conjunction on the timeField, and only uses $gte and $lt, we expect no
// residual filter.
(function testPipelineAddField_ConjunctionNoFilter() {
    const query = {$and: [{[timeFieldName]: {$lt: times[3]}}, {[timeFieldName]: {$gte: times[0]}}]};
    testUpdate({
        timeseriesOptions: fixedBucketOptions,
        initialDocList: [
            doc_id_9_time_string_metric,
            doc_id_10_time_array_metric,
            doc_id_11_time_no_metric,
            doc_id_12_time_int_metric
        ],
        updateList: [{
            q: query,
            u: [{$set: {newField: true}}],
            multi: true,
        }],
        resultDocList: [
            doc_id_10_time_array_metric,
            doc_id_12_time_int_metric,
            {
                _id: 9,
                [timeFieldName]: times[0],
                [metaFieldName]: {a: "A", c: "C"},
                f: "hello",
                newField: true
            },
            {_id: 11, [timeFieldName]: times[2], [metaFieldName]: {a: "A", c: "C"}, newField: true},
        ],
        nMatched: 2,
    });
})();

// Since the predicate doesn't align with the bucket boundaries, we expect a residual filter.
// We will remove the metaField.
(function testRemoveMetaField_Filter() {
    testUpdate({
        timeseriesOptions: fixedBucketOptions,
        initialDocList:
            [doc_id_10_time_array_metric, doc_id_11_time_no_metric, doc_id_12_time_int_metric],
        updateList: [{
            q: {[timeFieldName]: {$gte: times[2]}},
            u: {$unset: {[metaFieldName]: ""}},
            multi: true,
        }],
        resultDocList: [
            doc_id_10_time_array_metric,
            {_id: 11, [timeFieldName]: times[2]},
            {_id: 12, [timeFieldName]: times[3], f: 2}
        ],
        nMatched: 2,
    });
})();
