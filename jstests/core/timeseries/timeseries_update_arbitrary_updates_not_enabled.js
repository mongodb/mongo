/**
 * Tests running the update command on a time-series collection when the feature flag for arbitrary
 * time-series updates is not enabled. Split off from jstests/core/timeseries_update, see
 * SERVER-78202 for more context.
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns and tenant
 *   # migrations may result in writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # Specifically testing multi-updates.
 *   requires_multi_updates,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   # Specifically testing that certain commands should not work when the feature flag for
 *   # time-series arbitrary updates is not enabled.
 *   featureFlagTimeseriesUpdatesSupport_incompatible,
 * ]
 */
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

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

    // Multiple updates, ordered: Query on the metaField and modify the metaField and a field
    // that is not the metaField using dot notation.
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

    // Multiple updates, ordered: Modify the metaField, a field that is not the metaField, and
    // the metaField. Only the first update should succeed.
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

    // Multiple updates, unordered: Modify the metaField, a field that is not the metaField, and
    // the metaField. The first and last updates should succeed.
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

    // Multiple updates, ordered: Modify the metaField, a field that is not the metaField, and
    // the metaField. Only the first update should succeed.
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

    // Multiple updates, unordered: Modify the metaField, a field that is not the metaField, and
    // the metaField.
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
    // Query for documents using $jsonSchema with the metaField required and a required
    // subfield of the metaField with the same name as the metaField.
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

    // Updates where upsert:false should not insert a new document when no match is found but with
    // upsert:true, which should fail.
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

    /************************** Tests updating with an update pipeline ************************/
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

    /************************ Tests updating with a replacement document **********************/
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

    // Replace a document with an empty document, which should fail since updates with
    // replacement documents are not supported.
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
            u: {$set: {f: "FF"}},
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
            u: {$set: {f: "FF"}},
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
            u: {$set: {f: "FF"}},
            multi: true,
        }],
        resultDocList: [doc3],
        n: 0,
        failCode: ErrorCodes.InvalidOptions,
        hasMetaField: false,
    });
});
