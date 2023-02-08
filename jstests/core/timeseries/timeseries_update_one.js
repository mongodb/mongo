/**
 * Tests singleton updates on a time-series collection.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   featureFlagTimeseriesUpdatesDeletesSupport,
 *   # TODO (SERVER-73726): Re-enable the time-series updateOne test.
 *   __TEMPORARILY_DISABLED__,
 * ]
 */

(function() {
"use strict";

const timeFieldName = "time";
const metaFieldName = "mm";
const collNamePrefix = "timeseries_update_one_";
let count = 0;
const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

/**
 * Ensure the updateOne command operates correctly by examining documents after the update.
 */
function testUpdateOne({initialDocList, updateQuery, updateObj, resultDocList, n, upsert = false}) {
    const coll = testDB.getCollection(collNamePrefix + count++);
    if (initialDocList) {
        assert.commandWorked(testDB.createCollection(
            coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        assert.commandWorked(coll.insert(initialDocList));
    }

    const updateCommand = {
        update: coll.getName(),
        updates: [{q: updateQuery, u: updateObj, multi: false, upsert: upsert}]
    };
    const res = assert.commandWorked(testDB.runCommand(updateCommand));
    assert.eq(n, res.n);
    assert.eq((upsert) ? n - 1 : n, res.nModified);

    if (resultDocList) {
        const resDocs = coll.find().toArray();
        assert.eq(resDocs.length, resultDocList.length);
        resultDocList.forEach(resultDoc => {
            assert.docEq(resultDoc,
                         coll.findOne({_id: resultDoc._id}),
                         "Expected document " + resultDoc["_id"] +
                             " not found in result collection:" + tojson(resDocs));
        });
    }
}

/**
 * Tests op-style updates.
 */
{
    const doc_m1_a_b =
        {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), [metaFieldName]: 1, _id: 1, a: 1, b: 1};
    const doc_a_b = {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), _id: 1, a: 1, b: 1};
    const doc_m1_b =
        {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), [metaFieldName]: 1, _id: 1, b: 1};
    const doc_m2_b =
        {[timeFieldName]: ISODate("2023-02-06T19:19:01Z"), [metaFieldName]: 2, _id: 1, b: 1};
    const doc_m1_arrayA_b = {
        [timeFieldName]: ISODate("2023-02-06T19:19:01Z"),
        [metaFieldName]: 1,
        _id: 1,
        a: ["arr", "ay"],
        b: 1
    };
    const doc_stringM1_a_b = {
        [timeFieldName]: ISODate("2023-02-06T19:19:01Z"),
        [metaFieldName]: "1",
        _id: 1,
        a: 1,
        b: 1
    };
    const doc_m1_c_d =
        {[timeFieldName]: ISODate("2023-02-06T19:19:02Z"), [metaFieldName]: 1, _id: 2, c: 1, d: 1};
    const query_m1_a1 = {a: {$eq: 1}, [metaFieldName]: {$eq: 1}};
    const query_m1_b1 = {b: {$eq: 1}, [metaFieldName]: {$eq: 1}};

    // Metric field update: unset field.
    testUpdateOne({
        initialDocList: [doc_m1_a_b, doc_m1_c_d],
        updateQuery: query_m1_a1,
        updateObj: {$unset: {a: ""}},
        resultDocList: [doc_m1_b, doc_m1_c_d],
        n: 1
    });

    // Metric field update: add new field.
    testUpdateOne({
        initialDocList: [doc_m1_b, doc_m1_c_d],
        updateQuery: query_m1_b1,
        updateObj: {$set: {a: 1}},
        resultDocList: [doc_m1_a_b, doc_m1_c_d],
        n: 1
    });

    // Metric field update: change field type (integer to array).
    testUpdateOne({
        initialDocList: [doc_m1_a_b, doc_m1_c_d],
        updateQuery: query_m1_a1,
        updateObj: {$set: {a: ["arr", "ay"]}},
        resultDocList: [doc_m1_arrayA_b, doc_m1_c_d],
        n: 1
    });

    // Metric field update: no-op with non-existent field to unset.
    testUpdateOne({
        initialDocList: [doc_m1_a_b, doc_m1_c_d],
        updateQuery: query_m1_a1,
        updateObj: {$unset: {z: ""}},
        resultDocList: [doc_m1_a_b, doc_m1_c_d],
        n: 0
    });

    // Metric field update: no-op with non-existent field to unset.
    testUpdateOne({
        initialDocList: [doc_m1_a_b, doc_m1_c_d],
        updateQuery: {},
        updateObj: {$unset: {z: ""}},
        resultDocList: [doc_m1_a_b, doc_m1_c_d],
        n: 0
    });

    // Meta field update: remove meta field.
    testUpdateOne({
        initialDocList: [doc_m1_a_b, doc_m1_c_d],
        updateQuery: query_m1_a1,
        updateObj: {$unset: {[metaFieldName]: ""}},
        resultDocList: [doc_a_b, doc_m1_c_d],
        n: 1
    });

    // Meta field update: add meta field.
    testUpdateOne({
        initialDocList: [doc_a_b],
        updateQuery: {},
        updateObj: {$set: {[metaFieldName]: 1}},
        resultDocList: [doc_m1_a_b],
        n: 1
    });

    // Meta field update: add meta field.
    testUpdateOne({
        initialDocList: [doc_m1_b],
        updateQuery: {},
        updateObj: {$set: {[metaFieldName]: 2}},
        resultDocList: [doc_m2_b],
        n: 1
    });

    // Meta field update: update meta field to different type (integer to string).
    testUpdateOne({
        initialDocList: [doc_m1_a_b, doc_m1_c_d],
        updateQuery: query_m1_a1,
        updateObj: {$set: {[metaFieldName]: "1"}},
        resultDocList: [doc_stringM1_a_b, doc_m1_c_d],
        n: 1
    });
}

/**
 * Tests pipeline-style updates.
 */
{
    const timestamp2023 = ISODate("2023-02-06T19:19:00Z");
    const timestamp2022 = ISODate("2022-02-06T19:19:00Z");
    const doc_2023_m1_a1 = {[timeFieldName]: timestamp2023, [metaFieldName]: 1, _id: 1, a: 1};
    const doc_2022_m2_a1_newField =
        {[timeFieldName]: timestamp2022, [metaFieldName]: 2, _id: 1, a: 1, "newField": 42};

    // Update timeField, metaField and add a new field.
    testUpdateOne({
        initialDocList: [doc_2023_m1_a1],
        updateQuery: {a: {$eq: 1}, [metaFieldName]: {$eq: 1}},
        updateObj: [
            {$set: {[timeFieldName]: timestamp2022}},
            {$set: {[metaFieldName]: 2}},
            {$set: {"newField": 42}},
        ],
        resultDocList: [doc_2022_m2_a1_newField],
        n: 1
    });

    // Expect removal of the timeField to fail.
    assert.commandFailedWithCode(
        regColl.updateOne({}, [{$set: {[metaFieldName]: 2}}, {$unset: {[timeFieldName]: ""}}]),
        ErrorCodes.InvalidOptions);
}

/**
 * Tests full measurement replacement.
 */
{
    const timestamp2023 = ISODate("2023-02-06T19:19:00Z");
    const timestamp2022 = ISODate("2022-02-06T19:19:00Z");
    const doc_t2023_m1_id_a = {[timeFieldName]: timestamp2023, [metaFieldName]: 1, _id: 1, a: 1};
    const doc_t2022_m2_stringId_stringA =
        {[timeFieldName]: timestamp2022, [metaFieldName]: 2, "_id": 2, "a": 2};

    // Full measurement replacement: update every field in the document.
    testUpdateOne({
        initialDocList: [doc_t2023_m1_id_a],
        updateQuery: {},
        updateObj: doc_t2022_m2_stringId_stringA,
        resultDocList: [doc_t2022_m2_stringId_stringA],
        n: 1
    });

    // Tests upsert with full measurement.
    testUpdateOne({
        initialDocList: [doc_t2023_m1_id_a],
        updateQuery: {[metaFieldName]: {$eq: 2}},
        updateObj: doc_t2022_m2_stringId_stringA,
        resultDocList: [doc_t2023_m1_id_a, doc_t2022_m2_stringId_stringA],
        n: 1,
        upsert: true
    });

    // Tests upsert with full measurement: no-op when the query doesn't match and upsert is false.
    testUpdateOne({
        initialDocList: [doc_t2023_m1_id_a],
        updateQuery: {[metaFieldName]: {$eq: 2}},
        updateObj: doc_t2022_m2_stringId_stringA,
        resultDocList: [doc_t2023_m1_id_a],
        n: 0,
        upsert: false
    });
}

/**
 * Tests measurement modification that could exceed bucket size limit (default value of 128000
 * bytes).
 */
{
    const coll = testDB.getCollection(collNamePrefix + count++);
    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
    count--;  // Decrement count to access collection correctly in 'testUpdateOne' helper.

    // Fill up a bucket to roughly 120000 bytes by inserting 4 batches of 30 documents sized at
    // 1000 bytes.
    let batchNum = 0;
    while (batchNum < 4) {
        let batch = [];
        for (let i = 0; i < 30; i++) {
            const doc = {_id: i, [timeField]: ISODate(), value: "a".repeat(1000)};
            batch.push(doc);
        }

        assert.commandWorked(coll.insertMany(batch), {ordered: false});
        batchNum++;
    }

    // Update any of the measurements with a document which will exceed the 128000 byte threshold.
    const chunkyDoc = {_id: 128000, [timeField]: ISODate(), value: "a".repeat(10000)};
    testUpdateOne({
        // initialDocList: We manually inserted measurements.
        updateQuery: {},
        updateObj: chunkyDoc,
        // resultDocList: No need to check all of the measurements.
        n: 1
    });
}
})();
