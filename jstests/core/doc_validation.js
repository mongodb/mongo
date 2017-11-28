// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// Uses features that require featureCompatibilityVersion 3.6.
// @tags: [assumes_no_implicit_collection_creation_after_drop, requires_fcv36,
// requires_non_retryable_commands, requires_non_retryable_writes]

// Test basic inserts and updates with document validation.
(function() {
    "use strict";

    function assertFailsValidation(res) {
        if (res instanceof WriteResult) {
            assert.writeErrorWithCode(res, ErrorCodes.DocumentValidationFailure, tojson(res));
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.DocumentValidationFailure, tojson(res));
        }
    }

    const array = [];
    for (let i = 0; i < 2048; i++) {
        array.push({arbitrary: i});
    }

    const collName = "doc_validation";
    const coll = db[collName];

    /**
     * Runs a series of document validation tests using the validator 'validator', which should
     * enforce the existence of a field "a".
     */
    function runInsertUpdateValidationTest(validator) {
        coll.drop();

        // Create a collection with document validator 'validator'.
        assert.commandWorked(db.createCollection(collName, {validator: validator}));

        // Insert and upsert documents that will pass validation.
        assert.writeOK(coll.insert({_id: "valid1", a: 1}));
        assert.writeOK(coll.update({_id: "valid2"}, {_id: "valid2", a: 2}, {upsert: true}));
        assert.writeOK(coll.runCommand(
            "findAndModify", {query: {_id: "valid3"}, update: {$set: {a: 3}}, upsert: true}));

        // Insert and upsert documents that will not pass validation.
        assertFailsValidation(coll.insert({_id: "invalid3", b: 1}));
        assertFailsValidation(
            coll.update({_id: "invalid4"}, {_id: "invalid4", b: 2}, {upsert: true}));
        assertFailsValidation(coll.runCommand(
            "findAndModify", {query: {_id: "invalid4"}, update: {$set: {b: 3}}, upsert: true}));

        // Assert that we can remove the document that passed validation.
        assert.writeOK(coll.remove({_id: "valid1"}));

        // Check that we can only update documents that pass validation. We insert a valid and an
        // invalid document, then set the validator.
        coll.drop();
        assert.writeOK(coll.insert({_id: "valid1", a: 1}));
        assert.writeOK(coll.insert({_id: "invalid2", b: 1}));
        assert.commandWorked(coll.runCommand("collMod", {validator: validator}));

        // Assert that updates on a conforming document succeed when they affect fields not involved
        // in validator.
        // Add a new field.
        assert.writeOK(coll.update({_id: "valid1"}, {$set: {z: 1}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$set: {y: 2}}}));
        // In-place update.
        assert.writeOK(coll.update({_id: "valid1"}, {$inc: {z: 1}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$inc: {y: 1}}}));
        // Out-of-place update.
        assert.writeOK(coll.update({_id: "valid1"}, {$set: {z: array}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$set: {y: array}}}));
        // No-op update.
        assert.writeOK(coll.update({_id: "valid1"}, {a: 1}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$set: {a: 1}}}));

        // Verify those same updates will fail on non-conforming document.
        assertFailsValidation(coll.update({_id: "invalid2"}, {$set: {z: 1}}));
        assertFailsValidation(coll.update({_id: "invalid2"}, {$inc: {z: 1}}));
        assertFailsValidation(coll.update({_id: "invalid2"}, {$set: {z: array}}));
        assertFailsValidation(
            coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {y: 2}}}));
        assertFailsValidation(
            coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$inc: {y: 1}}}));
        assertFailsValidation(coll.runCommand(
            "findAndModify", {query: {_id: "invalid2"}, update: {$set: {y: array}}}));

        // A no-op update of an invalid doc will succeed.
        assert.writeOK(coll.update({_id: "invalid2"}, {$set: {b: 1}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {b: 1}}}));

        // Verify that we can't make a conforming document fail validation, but can update a
        // non-conforming document to pass validation.
        coll.drop();
        assert.writeOK(coll.insert({_id: "valid1", a: 1}));
        assert.writeOK(coll.insert({_id: "invalid2", b: 1}));
        assert.writeOK(coll.insert({_id: "invalid3", b: 1}));
        assert.commandWorked(coll.runCommand("collMod", {validator: validator}));

        assertFailsValidation(coll.update({_id: "valid1"}, {$unset: {a: 1}}));
        assert.writeOK(coll.update({_id: "invalid2"}, {$set: {a: 1}}));
        assertFailsValidation(
            coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$unset: {a: 1}}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "invalid3"}, update: {$set: {a: 1}}}));

        // Modify the collection to remove the document validator.
        assert.commandWorked(coll.runCommand("collMod", {validator: {}}));

        // Verify that no validation is applied to updates.
        assert.writeOK(coll.update({_id: "valid1"}, {$set: {z: 1}}));
        assert.writeOK(coll.update({_id: "invalid2"}, {$set: {z: 1}}));
        assert.writeOK(coll.update({_id: "valid1"}, {$unset: {a: 1}}));
        assert.writeOK(coll.update({_id: "invalid2"}, {$set: {a: 1}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$set: {z: 2}}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {z: 2}}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "valid1"}, update: {$unset: {a: 1}}}));
        assert.writeOK(
            coll.runCommand("findAndModify", {query: {_id: "invalid2"}, update: {$set: {a: 1}}}));
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
        assert.commandWorked(db.createCollection(
            collName, {validator: validator, collation: {locale: "en_US", strength: 2}}));

        // An insert that matches the validator should succeed.
        assert.writeOK(coll.insert({_id: 0, a: "xyz", b: "foo"}));

        const isJSONSchema = validator.hasOwnProperty("$jsonSchema");

        // A normal validator should respect the collation and the inserts should succeed. A JSON
        // Schema validator ignores the collation and the inserts should fail.
        const assertCorrectResult =
            isJSONSchema ? res => assertFailsValidation(res) : res => assert.writeOK(res);
        assertCorrectResult(coll.insert({a: "XYZ"}));
        assertCorrectResult(coll.insert({a: "XyZ", b: "foo"}));
        assertCorrectResult(coll.update({_id: 0}, {a: "xyZ", b: "foo"}));
        assertCorrectResult(coll.update({_id: 0}, {$set: {a: "Xyz"}}));
        assertCorrectResult(
            coll.runCommand("findAndModify", {query: {_id: 0}, update: {a: "xyZ", b: "foo"}}));
        assertCorrectResult(
            coll.runCommand("findAndModify", {query: {_id: 0}, update: {$set: {a: "Xyz"}}}));

        // Test an insert and an update that should always fail.
        assertFailsValidation(coll.insert({a: "not xyz"}));
        assertFailsValidation(coll.update({_id: 0}, {$set: {a: "xyzz"}}));
        assertFailsValidation(
            coll.runCommand("findAndModify", {query: {_id: 0}, update: {$set: {a: "xyzz"}}}));

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
    assert.writeOK(coll.insert({a: 5}));
    assertFailsValidation(coll.insert({a: 4}));
    assert.commandWorked(
        db.runCommand({"collMod": collName, "validator": {$expr: {$eq: ["$a", 4]}}}));
    assert.writeOK(coll.insert({a: 4}));
    assertFailsValidation(coll.insert({a: 5}));

    // The validator supports $expr with the date extraction expressions (with a timezone
    // specified).
    coll.drop();
    assert.commandWorked(db.createCollection(collName, {
        validator:
            {$expr: {$eq: [1, {$dayOfMonth: {date: "$a", timezone: "America/New_York"}}]}}
    }));
    assert.writeOK(coll.insert({a: ISODate("2017-10-01T22:00:00")}));
    assertFailsValidation(coll.insert({a: ISODate("2017-10-01T00:00:00")}));

    // The validator supports $expr with a $dateToParts expression.
    coll.drop();
    assert.commandWorked(db.createCollection(collName, {
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
                      "millisecond": 0
                    },
                    {$dateToParts: {date: "$a", timezone: "America/New_York"}}
                ]
            }
        }
    }));
    assert.writeOK(coll.insert({a: ISODate("2017-10-01T22:00:00")}));
    assertFailsValidation(coll.insert({a: ISODate("2017-10-01T00:00:00")}));

    // The validator supports $expr with $dateToString expression.
    coll.drop();
    assert.commandWorked(db.createCollection(collName, {
        validator: {
            $expr: {
                $eq: [
                    "2017-07-04 14:56:42 +0000 (0 minutes)",
                    {
                      $dateToString: {
                          format: "%Y-%m-%d %H:%M:%S %z (%Z minutes)",
                          date: "$date",
                          timezone: "$tz"
                      }
                    }
                ]
            }
        }
    }));
    assert.writeOK(coll.insert({date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "UTC"}));
    assertFailsValidation(
        coll.insert({date: new ISODate("2017-07-04T14:56:42.911Z"), tz: "America/New_York"}));

    // The validator supports $expr with $dateFromParts expression.
    coll.drop();
    assert.commandWorked(db.createCollection(collName, {
        validator: {
            $expr: {
                $eq: [
                    ISODate("2016-12-31T15:00:00Z"),
                    {'$dateFromParts': {year: "$year", "timezone": "$timezone"}}
                ]
            }
        }
    }));
    assert.writeOK(coll.insert({_id: 0, year: 2017, month: 6, day: 19, timezone: "Asia/Tokyo"}));
    assertFailsValidation(
        coll.insert({_id: 1, year: 2022, month: 1, day: 1, timezone: "America/New_York"}));

    // The validator supports $expr with $dateFromString expression.
    coll.drop();
    assert.commandWorked(db.createCollection(collName, {
        validator: {
            $expr: {
                $eq: [
                    ISODate("2017-07-04T15:56:02Z"),
                    {'$dateFromString': {dateString: "$date", timezone: 'America/New_York'}}
                ]
            }
        }
    }));
    assert.writeOK(coll.insert({_id: 0, date: "2017-07-04T11:56:02"}));
    assertFailsValidation(coll.insert({_id: 1, date: "2015-02-02T11:00:00"}));

    // The validator can contain an $expr that may throw at runtime.
    coll.drop();
    assert.commandWorked(
        db.createCollection(collName, {validator: {$expr: {$eq: ["$a", {$divide: [1, "$b"]}]}}}));
    assert.writeOK(coll.insert({a: 1, b: 1}));
    let res = coll.insert({a: 1, b: 0});
    assert.writeError(res);
    assert.eq(res.getWriteError().code, 16608);
    assert.writeOK(coll.insert({a: -1, b: -1}));
})();
