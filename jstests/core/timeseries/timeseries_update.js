/**
 * Tests running the update command on a time-series collection.
 * @tags: [
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_multi_updates,
 *   tenant_migration_incompatible,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/libs/fixture_helpers.js");

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

if (FixtureHelpers.isMongos(db) &&
    !TimeseriesTest.shardedtimeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the sharded time-series feature flag is disabled");
    return;
}

if (FixtureHelpers.isMongos(db) &&
    !TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog(
        "Skipping test because the sharded time-series updates and deletes feature flag is disabled");
    return;
}

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    /**
     * Confirms that a set of updates returns the expected set of documents.
     */
    function testUpdate({
        initialDocList,
        updateList,
        resultDocList,
        n,
        nModified = n,
        letDoc = {},
        failCode = null,
        hasMetaField = true,
        ordered = true
    }) {
        const coll = testDB.getCollection('t');
        assert.commandWorked(testDB.createCollection(coll.getName(), {
            timeseries: hasMetaField ? {timeField: timeFieldName, metaField: metaFieldName}
                                     : {timeField: timeFieldName},
        }));

        assert.commandWorked(insert(coll, initialDocList));

        const updateCommand =
            {update: coll.getName(), updates: updateList, ordered: ordered, let : letDoc};
        const res = failCode
            ? assert.commandFailedWithCode(testDB.runCommand(updateCommand), failCode)
            : assert.commandWorked(testDB.runCommand(updateCommand));

        assert.eq(n, res.n);
        assert.eq(nModified, res.nModified);
        assert.eq(initialDocList.length, resultDocList.length);

        resultDocList.forEach(resultDoc => {
            assert.docEq(resultDoc,
                         coll.findOne({_id: resultDoc._id}),
                         "Expected document not found in result collection:" +
                             tojson(coll.find().toArray()));
        });

        assert(coll.drop());
    }

    const doc1 = {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {a: "A", b: "B"}};
    const doc2 = {
        _id: 2,
        [timeFieldName]: dateTime,
        [metaFieldName]: {c: "C", d: 2},
        f: [{"k": "K", "v": "V"}]
    };
    const doc3 = {_id: 3, [timeFieldName]: dateTime, f: "F"};
    const doc4 = {_id: 4, [timeFieldName]: dateTime, [metaFieldName]: {a: "A", b: "B"}, f: "F"};
    const doc5 = {_id: 5, [timeFieldName]: dateTime, [metaFieldName]: {a: "A", b: "B", c: "C"}};

    const arrayDoc1 = {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: [1, 4, 7, 11, 13]};
    const arrayDoc2 = {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: [2, 5, 9, 12]};
    const arrayDoc3 = {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: [3, 6, 10]};

    /************************************ multi:false updates ************************************/
    testUpdate({
        initialDocList: [doc1],
        updateList: [{q: {[metaFieldName]: {b: "B"}}, u: {$set: {[metaFieldName]: {b: "C"}}}}],
        resultDocList: [doc1],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    /************************************ multi:true updates *************************************/
    /************************** Tests updating with an update document ***************************/
    // Query on the metaField and modify the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {[metaFieldName]: {a: "A", b: "B"}},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "C"}}],
        n: 1,
    });

    // Query on the metaField and modify the metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {$set: {[metaFieldName]: {e: "E"}}},
            multi: true,
        }],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {e: "E"},
            f: [{"k": "K", "v": "V"}]
        }],
        n: 1,
    });

    // Query on the metaField and modify the metaField of 1 matching document.
    testUpdate({
        initialDocList: [doc1, doc2, doc4, doc5],
        updateList: [{
            q: {"$and": [{[metaFieldName + ".c"]: "C"}, {[metaFieldName + ".d"]: 2}]},
            u: {$set: {[metaFieldName + ".c"]: 1}},
            multi: true,
        }],
        resultDocList: [
            doc1,
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: {c: 1, d: 2},
                f: [{"k": "K", "v": "V"}]
            },
            doc4,
            doc5
        ],
        ordered: false,
        n: 1,
    });

    // Query on the metaField and update the metaField of multiple matching documents.
    testUpdate({
        initialDocList: [doc1, doc2, doc4, doc5],
        updateList: [{
            q: {[metaFieldName + ".a"]: "A"},
            u: {$unset: {[metaFieldName + ".a"]: ""}},
            multi: true,
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {b: "B"}},
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: {c: "C", d: 2},
                f: [{"k": "K", "v": "V"}]
            },
            {_id: 4, [timeFieldName]: dateTime, [metaFieldName]: {b: "B"}, f: "F"},
            {_id: 5, [timeFieldName]: dateTime, [metaFieldName]: {b: "B", c: "C"}}
        ],
        ordered: false,
        n: 3,
    });

    testUpdate({
        initialDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 200, f: "F"}, doc3],
        updateList: [{
            q: {[metaFieldName]: {$nin: [5, 15]}},
            u: {$mul: {[metaFieldName]: 10}},
            multi: true,
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 2000, f: "F"},
            {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: 0, f: "F"}
        ],
        n: 2,
    });

    testUpdate({
        initialDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 200, f: "F"}, doc3],
        updateList: [{
            q: {[metaFieldName]: {$exists: true, $nin: [5, 15]}},
            u: {$mul: {[metaFieldName]: 10}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 2000, f: "F"}, doc3],
        n: 1,
    });

    // Query on a field that is not the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {measurement: "cpu"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [doc1],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Query on the metaField and modify a field that is not the metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {$set: {f2: "f2"}},
            multi: true,
        }],
        resultDocList: [doc2],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Query on the metaField and a field that is not the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [
            {
                q: {[metaFieldName]: {a: "A", b: "B"}, measurement: "cpu"},
                u: {$set: {[metaFieldName]: {c: "C"}}},
                multi: true,
            },
        ],
        resultDocList: [doc1],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Query on the metaField and modify the metaField and fields that are not the metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {$set: {[metaFieldName]: {e: "E"}, f3: "f3"}, $inc: {f2: 3}, $unset: {f1: ""}},
            multi: true,
        }],
        resultDocList: [doc2],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Compound query on the metaField using dot notation and modify the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {"$and": [{[metaFieldName + ".a"]: "A"}, {[metaFieldName + ".b"]: "B"}]},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "C"}}],
        n: 1,
    });

    // Query on the metaField using dot notation and modify the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {[metaFieldName + ".a"]: "A"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "C"}}],
        n: 1,
    });

    // Query on the metaField using dot notation and modify the metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName + ".c"]: "C"},
            u: {$inc: {[metaFieldName + ".d"]: 10}},
            multi: true,
        }],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", d: 12},
            f: [{"k": "K", "v": "V"}]
        }],
        n: 1,
    });

    // Query on a field that is not the metaField using dot notation and modify the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {"measurement.A": "cpu"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [doc1],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Query with an empty document (i.e update all documents in the collection).
    testUpdate({
        initialDocList: [doc1, doc2],
        updateList: [{
            q: {},
            u: {$set: {[metaFieldName]: {z: "Z"}}},
            multi: true,
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {z: "Z"}},
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: {z: "Z"},
                f: [{"k": "K", "v": "V"}]
            }
        ],
        n: 2,
    });

    // Remove the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {[metaFieldName]: {a: "A", b: "B"}},
            u: {$unset: {[metaFieldName]: ""}},
            multi: true
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime}],
        n: 1,
    });

    // Rename the metaField.
    testUpdate({
        initialDocList: [doc1, doc2, doc4],
        updateList: [{
            q: {[metaFieldName + ".a"]: "A"},
            u: {$rename: {[metaFieldName]: "Z"}},
            multi: true,
        }],
        resultDocList: [doc1, doc2, doc4],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Rename a subfield of the metaField.
    testUpdate({
        initialDocList: [doc1, doc2],
        updateList: [{
            q: {[metaFieldName + ".a"]: "A"},
            u: {$rename: {[metaFieldName + ".a"]: metaFieldName + ".z"}},
            multi: true,
        }],
        resultDocList:
            [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {z: "A", b: "B"}}, doc2],
        n: 1,
    });

    // Rename a subfield of the metaField to something not in the metaField.
    testUpdate({
        initialDocList: [doc1, doc2, doc4],
        updateList: [{
            q: {[metaFieldName + ".a"]: "A"},
            u: {$rename: {[metaFieldName + ".a"]: "notMetaField.a"}},
            multi: true,
        }],
        resultDocList: [doc1, doc2, doc4],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // For all documents that have at least one 2 in its metaField array, update the first 2
    // to be 100 using the positional $ operator.
    testUpdate({
        initialDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: [1, 2, 2]},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: [2, 3, 4]}
        ],
        updateList: [{
            q: {[metaFieldName]: 2},
            u: {$set: {[metaFieldName + ".$"]: 1000}},
            multi: true,
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: [1, 1000, 2]},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: [1000, 3, 4]}
        ],
        n: 2,
    });

    // Decrement (i.e. increment by a negative amount) all elements in the metaField array using the
    // positional $[] operator.
    testUpdate({
        initialDocList: [arrayDoc2, arrayDoc3],
        updateList: [{
            q: {},
            u: {$inc: {[metaFieldName + ".$[]"]: -3}},
            multi: true,
        }],
        resultDocList: [
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: [-1, 2, 6, 9]},
            {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: [0, 3, 7]}
        ],
        n: 2,
    });

    // Remove elements from all metaField arrays that match a condition.
    testUpdate({
        initialDocList: [arrayDoc1, arrayDoc2],
        updateList: [{
            q: {},
            u: {$pull: {[metaFieldName]: {$gt: 10}}},
            multi: true,
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: [1, 4, 7]},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: [2, 5, 9]}
        ],
        n: 2,
    });

    // Multiple updates, ordered: Remove elements from the metaField array that match a condition,
    // and then add elements to the metaField array.
    testUpdate({
        initialDocList: [arrayDoc2, arrayDoc3],
        updateList: [{
            q: {},
            u: {$pull: {[metaFieldName]: {$in: [1, 2, 3, 4, 5]}}},
            multi: true,
        }],
        resultDocList: [
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: [9, 12]},
            {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: [6, 10]}
        ],
        n: 2,
    });

    // Multiple updates, ordered: Query on the metaField and modify the metaField multiple times.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$set: {[metaFieldName]: 1}},
                multi: true,
            },
            {
                q: {[metaFieldName]: 1},
                u: {$set: {[metaFieldName]: 2}},
                multi: true,
            },
            {
                q: {[metaFieldName]: 2},
                u: {$set: {[metaFieldName]: 3}},
                multi: true,
            }
        ],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: 3,
            f: [{"k": "K", "v": "V"}],
        }],
        n: 3,
    });

    testUpdate({
        initialDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 200, f: "F"}],
        updateList: [
            {q: {}, u: {$min: {[metaFieldName]: 180}}, multi: true},
            {q: {}, u: {$max: {[metaFieldName]: 190}}, multi: true},
            {q: {}, u: {$mul: {[metaFieldName]: 3}}, multi: true}
        ],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: 570, f: "F"}],
        n: 3,
    });

    // Multiple updates, ordered: Query on the metaField and modify the metaField multiple times.
    testUpdate({
        initialDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {arr: [3, 6, 10], f: 10}},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {arr: [1, 2], f: 3}}
        ],
        updateList: [
            {
                q: {[metaFieldName + ".arr"]: {$ne: 7}},
                u: {$pop: {[metaFieldName + ".arr"]: 1}},
                multi: true,
            },
            {
                q: {[metaFieldName + ".f"]: {$mod: [2, 1]}},
                u: {$pop: {[metaFieldName + ".arr"]: -1}},
                multi: true,
            }
        ],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {arr: [3, 6], f: 10}},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: {arr: [], f: 3}}
        ],
        n: 3,
    });

    testUpdate({
        initialDocList: [doc1],
        updateList: [
            {
                q: {[metaFieldName]: {a: "A", b: "B"}},
                u: {$set: {[metaFieldName]: {c: "C", d: 1}}},
                multi: true,
            },
            {
                q: {[metaFieldName + ".d"]: 1},
                u: {$set: {[metaFieldName + ".c"]: "CC"}},
                multi: true,
            }
        ],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "CC", d: 1}}],
        n: 2,
    });

    // Multiple updates, ordered: Query on the metaField and on a field that is not the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [
            {
                q: {[metaFieldName]: {a: "A", b: "B"}},
                u: {$set: {[metaFieldName]: {c: "C", d: 1}}},
                multi: true,
            },
            {
                q: {measurement: "cpu", [metaFieldName + ".d"]: 1},
                u: {$set: {[metaFieldName + ".c"]: "CC"}},
                multi: true,
            }
        ],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "C", d: 1}}],
        n: 1,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, ordered: Query on the metaField and modify the metaField and a field that
    // is not the metaField using dot notation.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$inc: {[metaFieldName + ".d"]: 6}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            }
        ],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", d: 8},
            f: [{"k": "K", "v": "V"}],
        }],
        n: 1,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, ordered: Query on the metaField and modify a field that is not the
    // metaField using dot notation.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$inc: {[metaFieldName + ".d"]: 6}},
                multi: true,
            }
        ],
        resultDocList: [doc2],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, unordered: Query on the metaField and modify a field that is not the
    // metaField using dot notation.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$inc: {[metaFieldName + ".d"]: 6}},
                multi: true,
            }
        ],
        ordered: false,
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", d: 8},
            f: [{"k": "K", "v": "V"}],
        }],
        n: 1,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, ordered: Modify the metaField, a field that is not the metaField, and the
    // metaField. Only the first update should succeed.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$inc: {[metaFieldName + ".d"]: 6}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$inc: {[metaFieldName + ".d"]: 7}},
                multi: true,
            }
        ],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", d: 8},
            f: [{"k": "K", "v": "V"}],
        }],
        n: 1,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, unordered: Modify the metaField, a field that is not the metaField, and the
    // metaField. The first and last updates should succeed.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$inc: {[metaFieldName + ".d"]: 6}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$inc: {[metaFieldName + ".d"]: 7}},
                multi: true,
            }
        ],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", d: 15},
            f: [{"k": "K", "v": "V"}],
        }],
        ordered: false,
        n: 2,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, unordered: Query on the metaField and modify a field that is not the
    // metaField using dot notation.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$inc: {[metaFieldName + ".d"]: 6}},
                multi: true,
            }
        ],
        ordered: false,
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", d: 8},
            f: [{"k": "K", "v": "V"}],
        }],
        n: 1,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, ordered: Modify the metaField, a field that is not the metaField, and the
    // metaField. Only the first update should succeed.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$inc: {[metaFieldName + ".d"]: 6}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$inc: {[metaFieldName + ".d"]: 7}},
                multi: true,
            }
        ],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", d: 8},
            f: [{"k": "K", "v": "V"}],
        }],
        n: 1,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple updates, unordered: Modify the metaField, a field that is not the metaField, and the
    // metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [
            {
                q: {[metaFieldName]: {c: "C", d: 2}},
                u: {$inc: {[metaFieldName + ".d"]: 6}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$set: {"f1.0": "f2"}},
                multi: true,
            },
            {
                q: {[metaFieldName]: {c: "C", d: 8}},
                u: {$inc: {[metaFieldName + ".d"]: 7}},
                multi: true,
            }
        ],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", d: 15},
            f: [{"k": "K", "v": "V"}],
        }],
        ordered: false,
        n: 2,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple unordered updates on multiple matching documents.
    testUpdate({
        initialDocList: [doc1, doc2, doc4, doc5],
        updateList: [
            {
                q: {[metaFieldName + ".a"]: "A"},
                u: {$unset: {[metaFieldName + ".a"]: ""}},
                multi: true,
            },
            {
                q: {[metaFieldName + ".c"]: "C"},
                u: {$set: {[metaFieldName + ".d"]: 5}},
                multi: true,
            }
        ],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {b: "B"}},
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: {c: "C", d: 5},
                f: [{"k": "K", "v": "V"}]
            },
            {_id: 4, [timeFieldName]: dateTime, [metaFieldName]: {b: "B"}, f: "F"},
            {_id: 5, [timeFieldName]: dateTime, [metaFieldName]: {b: "B", c: "C", d: 5}}
        ],
        ordered: false,
        n: 5,
    });

    // Update where one of the matching documents is a no-op update.
    testUpdate({
        initialDocList: [doc1, doc4, doc5],
        updateList: [
            {
                q: {[metaFieldName + ".a"]: "A"},
                u: {$set: {[metaFieldName + ".c"]: "C"}},
                multi: true,
            },
        ],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {a: "A", b: "B", c: "C"}},
            {_id: 4, [timeFieldName]: dateTime, [metaFieldName]: {a: "A", b: "B", c: "C"}, f: "F"},
            {_id: 5, [timeFieldName]: dateTime, [metaFieldName]: {a: "A", b: "B", c: "C"}}
        ],
        n: 3,
        nModified: 2,
    });

    // Query for documents using $jsonSchema with the metaField required.
    testUpdate({
        initialDocList: [doc1, doc2, doc3],
        updateList: [{
            q: {"$jsonSchema": {"required": [metaFieldName]}},
            u: {$set: {[metaFieldName]: "a"}},
            multi: true
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: "a"},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: "a", f: [{"k": "K", "v": "V"}]},
            doc3
        ],
        n: 2
    });

    // Query for documents using $jsonSchema with the metaField in dot notation required.
    testUpdate({
        initialDocList: [doc1, doc2, doc3],
        updateList: [{
            q: {"$jsonSchema": {"required": [metaFieldName + ".a"]}},
            u: {$set: {[metaFieldName]: "a"}},
            multi: true
        }],
        resultDocList: [doc1, doc2, doc3],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Query for documents using $jsonSchema with a field that is not the metaField required.
    testUpdate({
        initialDocList: [doc1, doc2, doc3],
        updateList: [{
            q: {"$jsonSchema": {"required": [metaFieldName, timeFieldName]}},
            u: {$set: {[metaFieldName]: "a"}},
            multi: true
        }],
        resultDocList: [doc1, doc2, doc3],
        n: 0,
        failCode: ErrorCodes.InvalidOptions
    });

    const nestedMetaObj =
        {_id: 6, [timeFieldName]: dateTime, [metaFieldName]: {[metaFieldName]: "A"}};

    // Query for documents using $jsonSchema with the metaField required and a required subfield of
    // the metaField with the same name as the metaField.
    testUpdate({
        initialDocList: [doc1, nestedMetaObj],
        updateList: [{
            q: {
                "$jsonSchema": {
                    "required": [metaFieldName],
                    "properties": {[metaFieldName]: {"required": [metaFieldName]}}
                }
            },
            u: {$set: {[metaFieldName]: "a"}},
            multi: true
        }],
        resultDocList: [doc1, {_id: 6, [timeFieldName]: dateTime, [metaFieldName]: "a"}],
        n: 1
    });

    // Query for documents using $jsonSchema with the metaField required and an optional field that
    // is not the metaField.
    testUpdate({
        initialDocList: [doc1, nestedMetaObj],
        updateList: [{
            q: {
                "$jsonSchema": {
                    "required": [metaFieldName],
                    "properties": {"measurement": {description: "can be any value"}}
                }
            },
            u: {$set: {[metaFieldName]: "a"}},
            multi: true
        }],
        resultDocList: [doc1, nestedMetaObj],
        n: 0,
        failCode: ErrorCodes.InvalidOptions
    });

    // Multiple updates, unordered: Modify the metaField of all documents using arrayFilters.
    testUpdate({
        initialDocList: [arrayDoc1, arrayDoc2, arrayDoc3],
        updateList: [{
            q: {},
            u: {$set: {[metaFieldName + ".$[i]"]: 100}},
            multi: true,
            arrayFilters: [{"i": {$gte: 7}}],
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: [1, 4, 100, 100, 100]},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: [2, 5, 100, 100]},
            {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: [3, 6, 100]}
        ],
        n: 3,
    });

    // Multiple updates, unordered: Modify the metaField of multiple documents using arrayFilters.
    testUpdate({
        initialDocList: [arrayDoc1, arrayDoc2, arrayDoc3],
        updateList: [{
            q: {[metaFieldName]: {$lt: 3}},
            u: {$inc: {[metaFieldName + ".$[i]"]: 2000}},
            multi: true,
            arrayFilters: [{$and: [{"i": {$gt: 6}}, {"i": {$lt: 15}}]}],
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: [1, 4, 2007, 2011, 2013]},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: [2, 5, 2009, 2012]},
            arrayDoc3
        ],
        n: 2,
    });

    testUpdate({
        initialDocList: [arrayDoc1, arrayDoc2, arrayDoc3],
        updateList: [{
            q: {[metaFieldName]: {$lt: 2}},
            u: {$unset: {[metaFieldName + ".$[i]"]: ""}},
            multi: true,
            arrayFilters: [{$or: [{"i": {$lt: 5}}, {"i": {$gt: 10}}]}],
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: [null, null, 7, null, null]},
            arrayDoc2,
            arrayDoc3
        ],
        n: 1,
    });

    testUpdate({
        initialDocList: [
            {
                _id: 1,
                [timeFieldName]: dateTime,
                [metaFieldName]: [{a: 0, b: 2, c: 4}, {a: 6, b: 8, c: 10}]
            },
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: [{a: 1, b: 3, c: 5}, {a: 7, b: 9, c: 11}, {a: 3, b: 6, c: 9}]
            }
        ],
        updateList: [{
            q: {},
            u: {$set: {[metaFieldName + ".$[i].c"]: 0}},
            multi: true,
            arrayFilters: [{"i.a": {$gt: 5}}],
        }],
        resultDocList: [
            {
                _id: 1,
                [timeFieldName]: dateTime,
                [metaFieldName]: [{a: 0, b: 2, c: 4}, {a: 6, b: 8, c: 0}]
            },
            {
                _id: 2,
                [timeFieldName]: dateTime,
                [metaFieldName]: [{a: 1, b: 3, c: 5}, {a: 7, b: 9, c: 0}, {a: 3, b: 6, c: 9}]
            }
        ],
        n: 2,
    });

    // Query for documents on the metaField with the metaField nested within nested operators.
    testUpdate({
        initialDocList: [doc1, doc2, doc3],
        updateList: [{
            q: {
                "$and": [
                    {
                        "$or": [
                            {[metaFieldName]: {"$ne": "B"}},
                            {[metaFieldName]: {"a": {"$eq": "B"}}}
                        ]
                    },
                    {[metaFieldName]: {a: "A", b: "B"}}
                ]
            },
            u: {$set: {[metaFieldName]: "a"}},
            multi: true
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: "a"}, doc2, doc3],
        n: 1
    });

    // Updates where upsert:false should not insert a new document when no match is found.
    testUpdate({
        initialDocList: [doc1, doc4, doc5],
        updateList: [{
            q: {[metaFieldName]: "Z"},
            u: {$set: {[metaFieldName]: 5}},
            multi: true,
        }],
        resultDocList: [doc1, doc4, doc5],
        n: 0,
    });

    // Do the same test case as above but with upsert:true, which should fail.
    testUpdate({
        initialDocList: [doc1, doc4, doc5],
        updateList: [{
            q: {[metaFieldName]: "Z"},
            u: {$set: {[metaFieldName]: 5}},
            multi: true,
            upsert: true,
        }],
        resultDocList: [doc1, doc4, doc5],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Variables defined in the let option can only be used in the update if the update is an
    // pipeline update. Since this update is an update document, the literal name of the variable
    // will be used in the update instead of the variable's value.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {[metaFieldName + ".a"]: "A"},
            u: {$set: {[metaFieldName]: "$$myVar"}},
            multi: true,
        }],
        letDoc: {myVar: "aaa"},
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: "$$myVar"}],
        n: 1,
    });

    /************************** Tests updating with an update pipeline **************************/
    // Modify the metaField, which should fail since update pipelines are not supported.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {},
            u: [
                {$addFields: {[metaFieldName + ".c"]: "C", [metaFieldName + ".e"]: "E"}},
                {$unset: metaFieldName + ".e"}
            ],
            multi: true,
        }],
        resultDocList: [doc1],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    /************************ Tests updating with a replacement document *************************/
    // Replace a document to have no metaField, which should fail since updates with replacement
    // documents are not supported.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {f2: {e: "E", f: "F"}, f3: 7},
            multi: true,
        }],
        resultDocList: [doc2],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    // Replace a document with an empty document, which should fail since updates with replacement
    // documents are not supported.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {},
            multi: true,
        }],
        resultDocList: [doc2],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
    });

    /*********************** Tests updating a collection with no metaField. **********************/
    // Query on a field which is not the (nonexistent) metaField.
    testUpdate({
        initialDocList: [doc3],
        updateList: [{
            q: {f: "F"},
            u: {},
            multi: true,
        }],
        resultDocList: [doc3],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
        hasMetaField: false,
    });

    // Query on all documents and update them to be empty documents.
    testUpdate({
        initialDocList: [doc3],
        updateList: [{
            q: {},
            u: {},
            multi: true,
        }],
        resultDocList: [doc3],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
        hasMetaField: false,
    });

    // Query on all documents and update them to be nonempty documents.
    testUpdate({
        initialDocList: [doc3],
        updateList: [{
            q: {},
            u: {f: "FF"},
            multi: true,
        }],
        resultDocList: [doc3],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
        hasMetaField: false,
    });

    /************************ Tests updating a collection using collation. ************************/
    const collationDoc1 = {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: "café"};
    const collationDoc2 = {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: "cafe"};
    const collationDoc3 = {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: "cafE"};

    // Query on the metaField and modify the metaField using collation with strength level 1.
    testUpdate({
        initialDocList: [collationDoc1, collationDoc2, collationDoc3],
        updateList: [{
            q: {[metaFieldName]: "cafe"},
            u: {$set: {[metaFieldName]: "Updated"}},
            multi: true,
            collation: {locale: "fr", strength: 1},
        }],
        resultDocList: [
            {_id: 1, [timeFieldName]: dateTime, [metaFieldName]: "Updated"},
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: "Updated"},
            {_id: 3, [timeFieldName]: dateTime, [metaFieldName]: "Updated"}
        ],
        n: 3,
    });

    // Query on the metaField and modify the metaField using collation with the default strength
    // (level 3).
    testUpdate({
        initialDocList: [collationDoc1, collationDoc2, collationDoc3],
        updateList: [{
            q: {[metaFieldName]: "cafe"},
            u: {$set: {[metaFieldName]: "Updated"}},
            multi: true,
            collation: {locale: "fr"},
        }],
        resultDocList: [
            collationDoc1,
            {_id: 2, [timeFieldName]: dateTime, [metaFieldName]: "Updated"},
            collationDoc3,
        ],
        n: 1,
    });
});
}());
