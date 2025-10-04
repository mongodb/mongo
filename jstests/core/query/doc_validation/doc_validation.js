// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [
//   assumes_no_implicit_collection_creation_after_drop,
//   requires_fastcount,
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
//   requires_fastcount,
// ]

// Test basic inserts and updates with document validation.
import {assertDocumentValidationFailure} from "jstests/libs/doc_validation_utils.js";

const collName = "doc_validation";
const coll = db[collName];

const array = [];
for (let i = 0; i < 2048; i++) {
    array.push({arbitrary: i});
}

/**
 * Runs a series of document validation tests using the validator 'validator', which should
 * enforce the existence of a field "a".
 */
function runInsertUpdateValidationTest(validator) {
    coll.drop();

    // Create a collection with document validator 'validator'.
    assert.commandWorked(db.createCollection(collName, {validator: validator}));
    // The default validation level/action are OMITTED from "listCollections".
    // (After a collMod they do appear, see below).
    let info = db.getCollectionInfos({name: collName})[0];
    assert.eq(info.options.validationLevel, undefined, info);
    assert.eq(info.options.validationAction, undefined, info);

    // Insert and upsert documents that will pass validation.
    assert.commandWorked(coll.insert({_id: "valid1", a: 1}));
    assert.commandWorked(coll.update({_id: "valid2"}, {_id: "valid2", a: 2}, {upsert: true}));
    assert.commandWorked(
        coll.runCommand("findAndModify", {query: {_id: "valid3"}, update: {$set: {a: 3}}, upsert: true}),
    );

    // Insert and upsert documents that will not pass validation.
    assertDocumentValidationFailure(coll.insert({_id: "invalid3", b: 1}), coll);
    assertDocumentValidationFailure(coll.update({_id: "invalid4"}, {_id: "invalid4", b: 2}, {upsert: true}), coll);
    assertDocumentValidationFailure(
        coll.runCommand("findAndModify", {query: {_id: "invalid4"}, update: {$set: {b: 3}}, upsert: true}),
        coll,
    );

    // Assert that we can remove the document that passed validation.
    assert.commandWorked(coll.remove({_id: "valid1"}));

    // Check that we can only update documents that pass validation. We insert a valid and an
    // invalid document, then set the validator.
    coll.drop();
    assert.commandWorked(coll.insert({_id: "valid1", a: 1}));
    assert.commandWorked(coll.insert({_id: "invalid2", b: 1}));
    assert.commandWorked(coll.runCommand("collMod", {validator: validator}));
    // After collMod, the default validation level/action are INCLUDED in "listCollections".
    info = db.getCollectionInfos({name: collName})[0];
    assert.eq(info.options.validationLevel, "strict", info);
    assert.eq(info.options.validationAction, "error", info);

    // Assert that updates on a conforming document succeed when they affect fields not involved
    // in validator.
    // Add a new field.
    assert.commandWorked(coll.update({_id: "valid1"}, {$set: {z: 1}}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$set: {y: 2}}}));
    // In-place update.
    assert.commandWorked(coll.update({_id: "valid1"}, {$inc: {z: 1}}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$inc: {y: 1}}}));
    // Out-of-place update.
    assert.commandWorked(coll.update({_id: "valid1"}, {$set: {z: array}}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$set: {y: array}}}));
    // No-op update.
    assert.commandWorked(coll.update({_id: "valid1"}, {a: 1}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$set: {a: 1}}}));

    // Verify those same updates will fail on non-conforming document.
    assertDocumentValidationFailure(coll.update({_id: "invalid2"}, {$set: {z: 1}}), coll);
    assertDocumentValidationFailure(coll.update({_id: "invalid2"}, {$inc: {z: 1}}), coll);
    assertDocumentValidationFailure(coll.update({_id: "invalid2"}, {$set: {z: array}}), coll);
    assertDocumentValidationFailure(
        coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {y: 2}}}),
        coll,
    );
    assertDocumentValidationFailure(
        coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$inc: {y: 1}}}),
        coll,
    );
    assertDocumentValidationFailure(
        coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {y: array}}}),
        coll,
    );

    // A no-op update of an invalid doc will succeed.
    assert.commandWorked(coll.update({_id: "invalid2"}, {$set: {b: 1}}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {b: 1}}}));

    // Verify that we can't make a conforming document fail validation, but can update a
    // non-conforming document to pass validation.
    coll.drop();
    assert.commandWorked(coll.insert({_id: "valid1", a: 1}));
    assert.commandWorked(coll.insert({_id: "invalid2", b: 1}));
    assert.commandWorked(coll.insert({_id: "invalid3", b: 1}));
    assert.commandWorked(coll.runCommand("collMod", {validator: validator}));

    assertDocumentValidationFailure(coll.update({_id: "valid1"}, {$unset: {a: 1}}), coll);
    assert.commandWorked(coll.update({_id: "invalid2"}, {$set: {a: 1}}));
    assertDocumentValidationFailure(
        coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$unset: {a: 1}}}),
        coll,
    );
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "invalid3"}, update: {$set: {a: 1}}}));

    // Modify the collection to remove the document validator.
    assert.commandWorked(coll.runCommand("collMod", {validator: {}}));

    // Verify that no validation is applied to updates.
    assert.commandWorked(coll.update({_id: "valid1"}, {$set: {z: 1}}));
    assert.commandWorked(coll.update({_id: "invalid2"}, {$set: {z: 1}}));
    assert.commandWorked(coll.update({_id: "valid1"}, {$unset: {a: 1}}));
    assert.commandWorked(coll.update({_id: "invalid2"}, {$set: {a: 1}}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$set: {z: 2}}}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {z: 2}}}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$unset: {a: 1}}}));
    assert.commandWorked(coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {a: 1}}}));
}

// Run the test with a normal validator.
runInsertUpdateValidationTest({a: {$exists: true}});

// Run the test again with an equivalent JSON Schema.
runInsertUpdateValidationTest({$jsonSchema: {required: ["a"]}});

/**
 * Run a series of document validation tests involving collation using the validator
 * 'validator', which should enforce that the field "a" has the value "xyz".
 */
function runCollationValidationTest(validator) {
    coll.drop();
    assert.commandWorked(
        db.createCollection(collName, {validator: validator, collation: {locale: "en_US", strength: 2}}),
    );

    // An insert that matches the validator should succeed.
    assert.commandWorked(coll.insert({_id: 0, a: "xyz", b: "foo"}));

    const isJSONSchema = validator.hasOwnProperty("$jsonSchema");

    // A normal validator should respect the collation and the inserts should succeed. A JSON
    // Schema validator ignores the collation and the inserts should fail.
    const assertCorrectResult = isJSONSchema
        ? (res) => assertDocumentValidationFailure(res, coll)
        : (res) => assert.commandWorked(res);
    assertCorrectResult(coll.insert({a: "XYZ"}));
    assertCorrectResult(coll.insert({a: "XyZ", b: "foo"}));
    assertCorrectResult(coll.update({_id: 0}, {a: "xyZ", b: "foo"}));
    assertCorrectResult(coll.update({_id: 0}, {$set: {a: "Xyz"}}));
    assertCorrectResult(coll.runCommand("findAndModify", {query: {_id: 0}, update: {a: "xyZ", b: "foo"}}));
    assertCorrectResult(coll.runCommand("findAndModify", {query: {_id: 0}, update: {$set: {a: "Xyz"}}}));

    // Test an insert and an update that should always fail.
    assertDocumentValidationFailure(coll.insert({a: "not xyz"}), coll);
    assertDocumentValidationFailure(coll.update({_id: 0}, {$set: {a: "xyzz"}}), coll);
    assertDocumentValidationFailure(
        coll.runCommand("findAndModify", {query: {_id: 0}, update: {$set: {a: "xyzz"}}}),
        coll,
    );

    // A normal validator expands leaf arrays, such that if "a" is an array containing "xyz", it
    // matches {a: "xyz"}. A JSON Schema validator does not expand leaf arrays and treats arrays
    // as a single array value.
    assertCorrectResult(coll.insert({a: ["xyz"]}));
    assertCorrectResult(coll.insert({a: ["XYZ"]}));
    assertCorrectResult(coll.insert({a: ["XyZ"], b: "foo"}));
}

runCollationValidationTest({a: "xyz"});
runCollationValidationTest({$jsonSchema: {properties: {a: {enum: ["xyz"]}}}});

// The validator is allowed to contain $expr.
coll.drop();
assert.commandWorked(db.createCollection(collName, {validator: {$expr: {$eq: ["$a", 5]}}}));
assert.commandWorked(coll.insert({a: 5}));
assertDocumentValidationFailure(coll.insert({a: 4}), coll);
assert.commandWorked(db.runCommand({"collMod": collName, "validator": {$expr: {$eq: ["$a", 4]}}}));
assert.commandWorked(coll.insert({a: 4}));
assertDocumentValidationFailure(coll.insert({a: 5}), coll);

// The validator will generate detailed errors when $expr throws.
assert.commandWorked(db.runCommand({"collMod": collName, "validator": {$expr: {$divide: [10, 0]}}}));
assertDocumentValidationFailure(coll.insert({a: 4}), coll);
assert.commandWorked(db.runCommand({"collMod": collName, "validator": {$nor: [{$expr: {$divide: [10, 0]}}]}}));
assertDocumentValidationFailure(coll.insert({a: 4}), coll);

// The validator supports $expr with the date extraction expressions (with a timezone
// specified).
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {
        validator: {$expr: {$eq: [1, {$dayOfMonth: {date: "$a", timezone: "America/New_York"}}]}},
    }),
);
assert.commandWorked(coll.insert({a: ISODate("2017-10-01T22:00:00")}));
assertDocumentValidationFailure(coll.insert({a: ISODate("2017-10-01T00:00:00")}), coll);

// The validator supports $expr with a $dateToParts expression.
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {
        validator: {
            $expr: {
                $eq: [
                    {
                        "year": 2017,
                        "month": 10,
                        "day": 1,
                        "hour": 18,
                        "minute": 0,
                        "second": 0,
                        "millisecond": 0,
                    },
                    {$dateToParts: {date: "$a", timezone: "America/New_York"}},
                ],
            },
        },
    }),
);
assert.commandWorked(coll.insert({a: ISODate("2017-10-01T22:00:00")}));
assertDocumentValidationFailure(coll.insert({a: ISODate("2017-10-01T00:00:00")}), coll);

// The validator supports $expr with $dateToString expression.
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {
        validator: {
            $expr: {
                $eq: [
                    "2017-07-04 14:56:42 +0000 (0 minutes)",
                    {
                        $dateToString: {
                            format: "%Y-%m-%d %H:%M:%S %z (%Z minutes)",
                            date: "$date",
                            timezone: "$tz",
                        },
                    },
                ],
            },
        },
    }),
);
assert.commandWorked(coll.insert({date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "UTC"}));
assertDocumentValidationFailure(
    coll.insert({date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "America/New_York"}),
    coll,
);

// The validator supports $expr with $dateFromParts expression.
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {
        validator: {
            $expr: {
                $eq: [ISODate("2016-12-31T15:00:00Z"), {"$dateFromParts": {year: "$year", "timezone": "$timezone"}}],
            },
        },
    }),
);
assert.commandWorked(coll.insert({_id: 0, year: 2017, month: 6, day: 19, timezone: "Asia/Tokyo"}));
assertDocumentValidationFailure(
    coll.insert({_id: 1, year: 2022, month: 1, day: 1, timezone: "America/New_York"}),
    coll,
);

// The validator supports $expr with $dateFromString expression.
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {
        validator: {
            $expr: {
                $eq: [
                    ISODate("2017-07-04T15:56:02Z"),
                    {"$dateFromString": {dateString: "$date", timezone: "America/New_York"}},
                ],
            },
        },
    }),
);
assert.commandWorked(coll.insert({_id: 0, date: "2017-07-04T11:56:02"}));
assertDocumentValidationFailure(coll.insert({_id: 1, date: "2015-02-02T11:00:00"}), coll);

// The validator can contain an $expr that may throw at runtime.
coll.drop();
assert.commandWorked(db.createCollection(collName, {validator: {$expr: {$eq: ["$a", {$divide: [1, "$b"]}]}}}));
assert.commandWorked(coll.insert({a: 1, b: 1}));
let res = coll.insert({a: 1, b: 0});
assert.writeError(res);
assert.eq(res.getWriteError().code, ErrorCodes.DocumentValidationFailure);
assert.commandWorked(coll.insert({a: -1, b: -1}));

// The validator can contain an $expr that respects the collation.
coll.drop();
// Create collection with validator that uses case insensitive collation.
assert.commandWorked(
    db.createCollection(collName, {
        validator: {$expr: {$eq: ["$a", "foobar"]}},
        collation: {locale: "en", strength: 2},
    }),
);
assert.commandWorked(coll.insert({a: "foobar"}));
assert.commandWorked(coll.insert({a: "fooBAR"}));

// Recreate a collection with a simple validator.
coll.drop();
assert.commandWorked(db.createCollection(collName, {validator: {a: 1}}));

// Insert a document that fails validation.
res = coll.insert({_id: 1, a: 2});

// Verify that the document validation error attribute 'failingDocumentId' equals to the document
// '_id' attribute.
assertDocumentValidationFailure(res, coll);
const errorInfo = res.getWriteError().errInfo;
const expectedError = {
    failingDocumentId: 1,
    details: {operatorName: "$eq", specifiedAs: {a: 1}, reason: "comparison failed", consideredValue: 2},
};
assert.docEq(expectedError, errorInfo, tojson(res));

// Insert a valid document.
assert.commandWorked(coll.insert({_id: 1, a: 1}));

// Issues the update command and returns the response.
function updateCommand(coll, query, update) {
    return coll.update(query, update);
}

// Issues the findAndModify command and returns the response.
function findAndModifyCommand(coll, query, update) {
    return coll.runCommand("findAndModify", {query: query, update: update});
}

for (const command of [updateCommand, findAndModifyCommand]) {
    // Attempt to update the document by replacing it with a document that does not pass validation.
    const res = command(coll, {}, {a: 2});

    // Verify that the document validation error attribute 'failingDocumentId' equals to the
    // document '_id' attribute.
    assertDocumentValidationFailure(res, coll);
    const errorInfo = (res instanceof WriteResult ? res.getWriteError() : res).errInfo;
    const expectedError = {
        failingDocumentId: 1,
        details: {
            operatorName: "$eq",
            specifiedAs: {a: 1},
            reason: "comparison failed",
            consideredValue: 2,
        },
    };
    assert.docEq(expectedError, errorInfo, tojson(res));
}
