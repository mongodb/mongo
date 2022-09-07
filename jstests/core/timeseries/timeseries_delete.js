/**
 * Tests running the delete command on a time-series collection.
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

load("jstests/core/timeseries/libs/timeseries.js");
load("jstests/core/timeseries/libs/timeseries.js");  // For 'TimeseriesTest'.
load("jstests/libs/fixture_helpers.js");             // For 'FixtureHelpers'.

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

if (FixtureHelpers.isMongos(db) &&
    TimeseriesTest.shardedtimeseriesCollectionsEnabled(db.getMongo()) &&
    !TimeseriesTest.shardedTimeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the sharded time-series feature flag is disabled");
    return;
}

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
        const res = expectedErrorCode
            ? assert.commandFailedWithCode(
                  testDB.runCommand(
                      {delete: coll.getName(), deletes: deleteQuery, ordered, let : letDoc}),
                  expectedErrorCode)
            : assert.commandWorked(testDB.runCommand(
                  {delete: coll.getName(), deletes: deleteQuery, ordered, let : letDoc}));
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

    // Query on a single field that is not the metaField.
    testDelete([objA], [objA], 0, [{q: {measurement: "cpu"}, limit: 0}], {
        expectedErrorCode: ErrorCodes.InvalidOptions
    });

    // Query on the "meta" field.
    testDelete([objA], [objA], 0, [{q: {"meta": "A"}, limit: 0}], {
        expectedErrorCode: ErrorCodes.InvalidOptions
    });

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

    // Query on a single field that is not the metaField using dot notation.
    testDelete([objA, objB, objC],
               [objA, objB, objC],
               0,
               [{q: {"measurement.A": "cpu"}, limit: 0}],
               {expectedErrorCode: ErrorCodes.InvalidOptions});

    // Multiple queries on a single field that is the metaField.
    testDelete([objA, objB, objC], [objB], 2, [
        {q: {[metaFieldName]: {a: "A"}}, limit: 0},
        {q: {"$or": [{[metaFieldName]: {d: "D"}}, {[metaFieldName]: {c: "C"}}]}, limit: 0}
    ]);

    // Multiple queries on both the metaField and a field that is not the metaField.
    testDelete([objB],
               [],
               1,
               [
                   {q: {[metaFieldName]: {b: "B"}}, limit: 0},
                   {q: {measurement: "cpu", [metaFieldName]: {b: "B"}}, limit: 0}
               ],
               {expectedErrorCode: ErrorCodes.InvalidOptions});

    // Multiple queries on a field that is not the metaField.
    testDelete([objA, objB, objC],
               [objA, objB, objC],
               0,
               [{q: {measurement: "cpu"}, limit: 0}, {q: {measurement: "cpu-1"}, limit: 0}],
               {expectedErrorCode: ErrorCodes.InvalidOptions});

    // Multiple queries on both the metaField and a field that is not the metaField.
    testDelete([objA, objB, objC],
               [],
               3,
               [
                   {q: {[metaFieldName]: {b: "B"}}, limit: 0},
                   {q: {[metaFieldName]: {a: "A"}}, limit: 0},
                   {q: {[metaFieldName]: {d: "D"}}, limit: 0},
                   {q: {measurement: "cpu", [metaFieldName]: {b: "B"}}, limit: 0}
               ],
               {expectedErrorCode: ErrorCodes.InvalidOptions});

    // Query on a single field that is the metaField using limit: 1.
    testDelete([objA, objB, objC],
               [objA, objB, objC],
               0,
               [{q: {[metaFieldName]: {a: "A"}}, limit: 1}],
               {expectedErrorCode: ErrorCodes.IllegalOperation});

    // Multiple unordered queries on both the metaField and a field that is not the metaField.
    testDelete([objA, objB, objC],
               [],
               3,
               [
                   {q: {measurement: "cpu", [metaFieldName]: {b: "B"}}, limit: 0},
                   {q: {[metaFieldName]: {b: "B"}}, limit: 0},
                   {q: {[metaFieldName]: {a: "A"}}, limit: 0},
                   {q: {[metaFieldName]: {d: "D"}}, limit: 0}
               ],
               {expectedErrorCode: ErrorCodes.InvalidOptions, ordered: false});

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

    // Query on a field that is the prefix of the metaField.
    testDelete([objA], [objA], 0, [{q: {[metaFieldName + "b"]: "A"}, limit: 0}], {
        expectedErrorCode: ErrorCodes.InvalidOptions
    });

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

    // Query for documents using $jsonSchema with the metaField in dot notation required.
    testDelete([nestedObjA, nestedObjB, nestedObjC],
               [nestedObjA, nestedObjB, nestedObjC],
               0,
               [{q: {"$jsonSchema": {"required": [metaFieldName + ".a"]}}, limit: 0}],
               {expectedErrorCode: ErrorCodes.InvalidOptions});

    // Query for documents using $jsonSchema with a field that is not the metaField required.
    testDelete([nestedObjA, nestedObjB, nestedObjC],
               [nestedObjA, nestedObjB, nestedObjC],
               0,
               [{q: {"$jsonSchema": {"required": [metaFieldName, "measurement"]}}, limit: 0}],
               {expectedErrorCode: ErrorCodes.InvalidOptions});

    const nestedMetaObj = {[timeFieldName]: ISODate(), [metaFieldName]: {[metaFieldName]: "A"}};

    // Query for documents using $jsonSchema with the metaField required and a required subfield of
    // the metaField with the same name as the metaField.
    testDelete([objA, nestedMetaObj], [objA], 1, [{
                   q: {
                       "$jsonSchema": {
                           "required": [metaFieldName],
                           "properties": {[metaFieldName]: {"required": [metaFieldName]}}
                       }
                   },
                   limit: 0
               }]);

    // Query for documents using $jsonSchema with the metaField required and an optional field that
    // is not the metaField.
    testDelete([objA, nestedMetaObj],
               [objA, nestedMetaObj],
               0,
               [{
                   q: {
                       "$jsonSchema": {
                           "required": [metaFieldName],
                           "properties": {"measurement": {description: "can be any value"}}
                       }
                   },
                   limit: 0
               }],
               {expectedErrorCode: ErrorCodes.InvalidOptions});

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

    // Query on the "meta" field.
    testDelete([objA],
               [objA],
               0,
               [{q: {"meta": "A"}, limit: 0}],
               {expectedErrorCode: ErrorCodes.InvalidOptions, includeMetaField: false});
});
})();
