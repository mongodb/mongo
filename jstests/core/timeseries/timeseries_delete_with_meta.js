/**
 * Tests running the delete command on a time-series collection. These commands operate on the full
 * bucket document by targeting them with their meta field value.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.
load("jstests/libs/analyze_plan.js");                // For planHasStage().
load("jstests/libs/fixture_helpers.js");             // For 'FixtureHelpers'.

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.getCollection('t');
const timeFieldName = "time";
const metaFieldName = "tag";

TimeseriesTest.run((insert) => {
    const testDelete = function(
        docsToInsert,
        expectedRemainingDocs,
        expectedNRemoved,
        deleteQuery,
        {expectedErrorCode = null, ordered = true, includeMetaField = true, letDoc = null} = {}) {
        assert.commandWorked(testDB.createCollection(coll.getName(), {
            timeseries: {
                timeField: timeFieldName,
                metaField: (includeMetaField ? metaFieldName : undefined)
            }
        }));

        docsToInsert.forEach(doc => {
            assert.commandWorked(insert(coll, doc));
        });

        const deleteCommand = {delete: coll.getName(), deletes: deleteQuery, ordered, let : letDoc};

        // Explain for delete command only works for single delete when the arbitrary timeseries
        // delete feature is enabled and we check whether the explain works only when it's supposed
        // to work without an error because we verify it with 'executionStats' explain.
        if (FeatureFlagUtil.isPresentAndEnabled(testDB, "TimeseriesDeletesSupport") &&
            deleteQuery.length === 1 && expectedErrorCode === null) {
            const explain = assert.commandWorked(
                testDB.runCommand({explain: deleteCommand, verbosity: "executionStats"}));
            jsTestLog(tojson(explain));
            assert(planHasStage(testDB, explain.queryPlanner.winningPlan, "BATCHED_DELETE") ||
                   planHasStage(testDB, explain.queryPlanner.winningPlan, "DELETE") ||
                   planHasStage(testDB, explain.queryPlanner.winningPlan, "TS_MODIFY"));
        }

        const res = expectedErrorCode
            ? assert.commandFailedWithCode(testDB.runCommand(deleteCommand), expectedErrorCode)
            : assert.commandWorked(testDB.runCommand(deleteCommand));
        const docs = coll.find({}, {_id: 0}).toArray();
        assert.eq(res["n"], expectedNRemoved);
        assert.sameMembers(docs, expectedRemainingDocs);
        assert(coll.drop());
    };

    /******************** Tests deleting from a collection with a metaField **********************/
    // Query on a single field that is the metaField.
    testDelete([{[timeFieldName]: ISODate(), [metaFieldName]: "A"}],
               [],
               1,
               [{q: {[metaFieldName]: "A"}, limit: 0}]);

    const objA =
        {[timeFieldName]: ISODate(), "measurement": {"A": "cpu"}, [metaFieldName]: {a: "A"}};

    // Query on a single field that is the metaField using dot notation.
    testDelete([objA], [], 1, [{q: {[metaFieldName + ".a"]: "A"}, limit: 0}]);
    testDelete([objA, objA], [], 2, [{q: {[metaFieldName + ".a"]: "A"}, limit: 0}]);

    // Compound query on a single field that is the metaField using dot notation.
    testDelete([{[timeFieldName]: ISODate(), [metaFieldName]: {"a": "A", "b": "B"}}], [], 1, [
        {q: {"$and": [{[metaFieldName + ".a"]: "A"}, {[metaFieldName + ".b"]: "B"}]}, limit: 0}
    ]);

    const objB =
        {[timeFieldName]: ISODate(), "measurement": {"A": "cpu"}, [metaFieldName]: {b: "B"}};
    const objC =
        {[timeFieldName]: ISODate(), "measurement": {"A": "cpu"}, [metaFieldName]: {d: "D"}};

    // Multiple queries on a single field that is the metaField.
    testDelete([objA, objB, objC], [objB], 2, [
        {q: {[metaFieldName]: {a: "A"}}, limit: 0},
        {q: {"$or": [{[metaFieldName]: {d: "D"}}, {[metaFieldName]: {c: "C"}}]}, limit: 0}
    ]);

    const nestedObjA =
        {[timeFieldName]: ISODate(), "measurement": {"A": "cpu"}, [metaFieldName]: {a: {b: "B"}}};
    const nestedObjB =
        {[timeFieldName]: ISODate(), "measurement": {"A": "cpu"}, [metaFieldName]: {b: {a: "A"}}};
    const nestedObjC =
        {[timeFieldName]: ISODate(), "measurement": {"A": "cpu"}, [metaFieldName]: {d: "D"}};

    // Query on a single nested field that is the metaField.
    testDelete([nestedObjA, nestedObjB, nestedObjC],
               [nestedObjB, nestedObjC],
               1,
               [{q: {[metaFieldName]: {a: {b: "B"}}}, limit: 0}]);

    // Query on a single nested field that is the metaField using dot notation.
    testDelete([nestedObjB, nestedObjC],
               [nestedObjC],
               1,
               [{q: {[metaFieldName + ".b.a"]: "A"}, limit: 0}]);

    const objACollation = {[timeFieldName]: ISODate(), [metaFieldName]: "Günter"};
    const objBCollation = {[timeFieldName]: ISODate(), [metaFieldName]: "Gunter"};
    // Query on the metaField using collation.
    testDelete([objACollation, objBCollation], [objBCollation], 1, [{
                   q: {[metaFieldName]: {"$lt": "Gunter"}},
                   limit: 0,
                   collation: {locale: "de@collation=phonebook"}
               }]);

    const dollarObjA = {
        [timeFieldName]: ISODate(),
        "measurement": {"A": "cpu"},
        [metaFieldName]: {b: "$" + metaFieldName}
    };

    // Query on the metaField for documents with "$" + the metaField as a field value.
    testDelete([dollarObjA], [], 1, [{q: {[metaFieldName]: {b: "$" + metaFieldName}}, limit: 0}]);

    // Query for documents using $jsonSchema with the metaField required.
    testDelete([nestedObjA, nestedObjB, nestedObjC],
               [],
               3,
               [{q: {"$jsonSchema": {"required": [metaFieldName]}}, limit: 0}]);

    const nestedMetaObj = {[timeFieldName]: ISODate(), [metaFieldName]: {[metaFieldName]: "A"}};

    // Query for documents using $jsonSchema with the metaField required and a required subfield
    // of the metaField with the same name as the metaField.
    testDelete([objA, nestedMetaObj], [objA], 1, [{
                   q: {
                       "$jsonSchema": {
                           "required": [metaFieldName],
                           "properties": {[metaFieldName]: {"required": [metaFieldName]}}
                       }
                   },
                   limit: 0
               }]);

    // Query on the metaField with the metaField nested within nested operators.
    testDelete([objA, objB, objC], [objB, objC], 1, [{
                   q: {
                       "$and": [
                           {
                               "$or": [
                                   {[metaFieldName]: {"$ne": "B"}},
                                   {[metaFieldName]: {"a": {"$eq": "B"}}}
                               ]
                           },
                           {[metaFieldName]: {"a": "A"}}
                       ]
                   },
                   limit: 0
               }]);

    /******************* Tests deleting from a collection without a metaField ********************/
    // Remove all documents.
    testDelete([{[timeFieldName]: ISODate(), "meta": "A"}], [], 1, [{q: {}, limit: 0}], {
        includeMetaField: false
    });
});
})();
