/**
 * Tests running the update command on a time-series collection.
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_50,
 *   requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

const timeFieldName = "time";
const metaFieldName = "tag";
const dateTime = ISODate("2021-07-12T16:00:00Z");
const doc1 = {
    _id: 1,
    [timeFieldName]: dateTime,
    [metaFieldName]: {a: "A", b: "B"}
};
const doc2 = {
    _id: 2,
    [timeFieldName]: dateTime,
    [metaFieldName]: {c: "C", d: 2},
    f: [{"k": "K", "v": "V"}],
};

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
        nModified = 0,
        failCode = null,
        ordered = true
    }) {
        const coll = testDB.getCollection('t');
        assert.commandWorked(testDB.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        assert.commandWorked(insert(coll, initialDocList));

        const updateCommand = {update: coll.getName(), updates: updateList};
        if (failCode) {
            assert.commandFailedWithCode(testDB.runCommand(updateCommand), failCode);
        } else {
            const res = assert.commandWorked(testDB.runCommand(updateCommand));
            assert.eq(res["n"], nModified);
        }

        assert.docEq(coll.find().toArray(), resultDocList);
        assert(coll.drop());
    }

    /************************************ multi:false updates ************************************/
    testUpdate({
        initialDocList: [doc1],
        updateList: [{q: {[metaFieldName]: {b: "B"}}, u: {$set: {[metaFieldName]: {b: "C"}}}}],
        resultDocList: [doc1],
        failCode: ErrorCodes.InvalidOptions,
    });

    /************************************ multi:true updates *************************************/
    // Query on a single field that is the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {[metaFieldName]: {a: "A", b: "B"}},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "C"}}],
        nModified: 1,
    });

    // Query on a single field that is not the metaField.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {measurement: "cpu"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [doc1],
        failCode: ErrorCodes.InvalidOptions,
    });

    // Query on both the metaField and a field that is not the metaField.
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
        failCode: ErrorCodes.InvalidOptions,
    });

    // Compound query on the metaField using dot notation.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {"$and": [{[metaFieldName + ".a"]: "A"}, {[metaFieldName + ".b"]: "B"}]},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "C"}}],
        nModified: 1,
    });

    // Query on a single field that is the metaField using dot notation.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {[metaFieldName + ".a"]: "A"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [{_id: 1, [timeFieldName]: dateTime, [metaFieldName]: {c: "C"}}],
        nModified: 1,
    });

    // Query on a single field that is not the metaField using dot notation.
    testUpdate({
        initialDocList: [doc1],
        updateList: [{
            q: {"measurement.A": "cpu"},
            u: {$set: {[metaFieldName]: {c: "C"}}},
            multi: true,
        }],
        resultDocList: [doc1],
        failCode: ErrorCodes.InvalidOptions,
    });

    // Multiple queries on a single field that is the metaField.
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
        resultDocList: [doc1],
        failCode: ErrorCodes.IllegalOperation,
    });

    // Multiple queries on both the metaField and a field that is not the metaField.
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
        resultDocList: [doc1],
        failCode: ErrorCodes.IllegalOperation,
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
        nModified: 2,
    });

    /************************** Tests updating with an update document ***************************/
    //  Modify a field that is the metaField.
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
        nModified: 1,
    });

    // Modify a field that is not the metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {$set: {f2: "f2"}},
            multi: true,
        }],
        resultDocList: [doc2],
        failCode: ErrorCodes.InvalidOptions,
    });

    // Modify the metafield and fields that are not the metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {$set: {[metaFieldName]: {e: "E"}, f3: "f3"}, $inc: {f2: 3}, $unset: {f1: ""}},
            multi: true,
        }],
        resultDocList: [doc2],
        failCode: ErrorCodes.InvalidOptions,
    });

    // Modify a field that is the metaField using dot notation.
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
        nModified: 1,
    });

    // Modify the metaField multiple times.
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
        resultDocList: [doc2],
        failCode: ErrorCodes.IllegalOperation,
    });

    // Modify the metaField and a field that is not the metaField using dot notation.
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
        resultDocList: [doc2],
        failCode: ErrorCodes.IllegalOperation,
    });

    /************************** Tests updating with an update pipeline **************************/
    // Modify the metaField using dot notation: Add embedded fields to the metaField and remove an
    // embedded field.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName + ".d"]: 2},
            u: [
                {$set: {[metaFieldName + ".e"]: "E", [metaFieldName + ".f"]: "F"}},
                {$unset: metaFieldName + ".d"}
            ],
            multi: true,
        }],
        resultDocList: [{
            _id: 2,
            [timeFieldName]: dateTime,
            [metaFieldName]: {c: "C", e: "E", f: "F"},
            f: [{"k": "K", "v": "V"}]
        }],
        nModified: 1,
    });

    // Modify the metaField using dot notation: Remove an embedded field of the metaField
    // and a field that is not the metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName + ".c"]: "C"},
            u: [{$unset: [metaFieldName + ".c", "f"]}],
            multi: true,
        }],
        resultDocList: [doc2],
        failCode: ErrorCodes.InvalidOptions,
    });

    // Modify the metaField using dot notation: Add an embedded field and add a new field.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName + ".c"]: "C"},
            u: [{$set: {[metaFieldName + ".e"]: "E", n: 1}}],
            multi: true,
        }],
        resultDocList: [doc2],
        failCode: ErrorCodes.InvalidOptions,
    });

    // Replace an entire document.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName + ".c"]: "C"},
            u: [{$replaceWith: {_id: 4, t: ISODate(), [metaFieldName]: {"z": "Z"}}}],
            multi: true,
        }],
        resultDocList: [doc2],
        failCode: ErrorCodes.InvalidOptions,
    });

    /************************ Tests updating with a replacement document *************************/
    // Replace a document to have no metaField.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {f2: {e: "E", f: "F"}, f3: 7},
            multi: true,
        }],
        resultDocList: [doc2],
        failCode: ErrorCodes.InvalidOptions,
    });

    // Replace a document with an empty document.
    testUpdate({
        initialDocList: [doc2],
        updateList: [{
            q: {[metaFieldName]: {c: "C", d: 2}},
            u: {},
            multi: true,
        }],
        resultDocList: [doc2],
        failCode: ErrorCodes.InvalidOptions,
    });
});
}());
