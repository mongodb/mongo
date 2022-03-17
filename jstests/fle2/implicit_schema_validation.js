// Verify implicit schema validation works for encrypted collections

/**
 * @tags: [
 *  featureFlagFLE2,
 * ]
 */
(function() {
'use strict';

const isFLE2Enabled = TestData == undefined || TestData.setParameters.featureFlagFLE2;

if (!isFLE2Enabled) {
    return;
}

const dbTest = db.getSiblingDB('implicit_schema_validation_db');

const validEncryptedString = HexData(6, "060102030405060708091011121314151602");
const validEncryptedInt = HexData(6, "060102030405060708091011121314151610");
const nonEncryptedBinData = HexData(3, "060102030405060708091011121314151610");
const fle1RandomBinData = HexData(6, "020102030405060708091011121314151602");
const fle2PlaceholderBinData = HexData(6, "030102030405060708091011121314151602");

const userMalformedSchema = {
    $or: [
        {name: {$not: {$foo: true}}},
        {name: {$type: "string"}},
    ]
};

const userQueryOpSchema = {
    $or: [
        {name: {$not: {$exists: true}}},
        {name: {$type: "string"}},
    ]
};

const userJsonSchema = {
    $jsonSchema: {
        bsonType: "object",
        properties: {
            name: {
                bsonType: "string",
            }
        }
    }
};

const userJsonConflictSchema = {
    $jsonSchema: {
        bsonType: "object",
        properties: {
            name: {bsonType: "string"},
            firstName: {bsonType: "string"},
        }
    }
};

const fle1Schema = {
    $jsonSchema: {
        bsonType: "object",
        properties: {
            name: {
                encrypt: {
                    algorithm: "AEAD_AES_256_CBC_HMAC_SHA_512-Random",
                    keyId: [UUID()],
                    bsonType: "string",
                }
            }
        }
    }
};

const sampleEncryptedFields = {
    fields: [
        {
            path: "firstName",
            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            bsonType: "string",
            queries: {"queryType": "equality"}
        },
        {
            path: "a.b.c",
            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-000000000001"),
            bsonType: "int",
            queries: {"queryType": "equality"}
        },
        {
            path: "a.b.d",
            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-000000000002"),
            bsonType: "int",
            queries: {"queryType": "equality"}
        },
        {
            path: "e.g",
            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-000000000003"),
            bsonType: "string",
            queries: {"queryType": "equality"}
        },
        {
            path: "a.x.y",
            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-000000000004"),
            bsonType: "string",
            queries: {"queryType": "equality"}
        },
    ]
};

// Tests invalid inserts on encrypted collection 'coll'.
// This assumes 'coll' was created encrypted fields specified in 'sampleEncryptedFields'.
// If 'hasUserValidator' is true, this assumes it validates the optional field 'name' is a string.
function negativeTests(coll, hasUserValidator, invert = false) {
    function assertExpectedResult(result) {
        if (invert) {
            assert.commandWorked(result);
        } else {
            assert.commandFailedWithCode(result, ErrorCodes.DocumentValidationFailure);
        }
    }

    jsTestLog("test inserting non-bindata value for encrypted field");
    assertExpectedResult(coll.insert({firstName: "foo"}));
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: validEncryptedInt,
                d: "foo",
            },
        }
    }));

    jsTestLog("test path to encrypted field has arrays");
    assertExpectedResult(coll.insert({a: [{b: {c: validEncryptedInt}}]}));
    assertExpectedResult(coll.insert({a: {b: [{c: validEncryptedInt}]}}));
    assertExpectedResult(coll.insert({a: {b: {c: []}}}));

    jsTestLog("test inserting encrypted field with BinData of incorrect subtype");
    assertExpectedResult(coll.insert({firstName: nonEncryptedBinData}));
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: nonEncryptedBinData,
                d: validEncryptedInt,
            },
        }
    }));

    jsTestLog("test inserting encrypted field with incorrect FLE2 subtype");
    assertExpectedResult(coll.insert({firstName: fle1RandomBinData}));
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: fle2PlaceholderBinData,
                d: validEncryptedInt,
            },
        }
    }));

    jsTestLog(
        "test inserting encrypted field with incorrect BSONType specifier for the unencrypted value");
    assertExpectedResult(coll.insert({firstName: validEncryptedInt}));
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: validEncryptedString,
                d: validEncryptedInt,
            },
        }
    }));

    if (!hasUserValidator) {
        return;
    }

    jsTestLog("test insert violating user-provided validator");
    assertExpectedResult(coll.insert({firstName: validEncryptedString, name: 234}));
    assertExpectedResult(coll.insert({firstName: nonEncryptedBinData, name: 234}));
}

// Tests invalid updates on encrypted collection 'coll'
// This assumes 'coll' was created encrypted fields specified in 'sampleEncryptedFields'.
function negativeUpdateTests(coll, invert = false) {
    function assertExpectedResult(result) {
        if (invert) {
            assert.commandWorked(result);
        } else {
            assert.commandFailedWithCode(result, ErrorCodes.DocumentValidationFailure);
        }
    }

    // first, insert a valid document to update
    assert.commandWorked(coll.insert({
        test_id: 0,
        firstName: validEncryptedString,
        a: {
            b: {
                c: validEncryptedInt,
                d: validEncryptedInt,
            },
            x: {
                y: validEncryptedString,
            }
        }
    }));

    jsTestLog("test updating encrypted field with invalid value");
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": "roger"}}));
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": nonEncryptedBinData}}));
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": fle1RandomBinData}}));
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": validEncryptedInt}}));
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.x.y": [1, 2, 3]}}));
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.x": {"y": 42}}}));

    jsTestLog("test updating prefix of encrypted field with array value");
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.b": [1, 2, 3]}}));
}

// Tests valid inserts on encrypted collection 'coll'.
// This assumes 'coll' was created encrypted fields specified in 'sampleEncryptedFields'.
// If 'hasUserValidator' is true, this assumes it validates the optional field 'name' is a string.
function positiveTests(coll, hasUserValidator, invert = false) {
    function assertExpectedResult(result) {
        if (invert) {
            assert.commandFailedWithCode(result, ErrorCodes.DocumentValidationFailure);
        } else {
            assert.commandWorked(result);
        }
    }

    jsTestLog("test inserting document without any encrypted fields");
    assert.commandWorked(coll.insert({}));
    assert.commandWorked(coll.insert({foo: 1}));
    assert.commandWorked(coll.insert({a: {foo: 1}}));
    assert.commandWorked(coll.insert({a: {b: {foo: 1}, x: {foo: 1}}}));

    jsTestLog("test inserting single encrypted field with valid type");
    assertExpectedResult(coll.insert({firstName: validEncryptedString}));

    jsTestLog("test inserting multiple encrypted fields with valid type");
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: validEncryptedInt,
                d: validEncryptedInt,
            },
            x: {
                y: validEncryptedString,
            }
        }
    }));

    jsTestLog("test inserting non-object along encrypted path");
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: "foo",
    }));
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: validEncryptedInt,
                d: validEncryptedInt,
            },
            x: "foo",
        }
    }));

    jsTestLog("test insert satisfies user-provided validator");
    assertExpectedResult(coll.insert({name: "joe", firstName: validEncryptedString}));
}

// Tests valid updates on encrypted collection 'coll'
// This assumes 'coll' was created encrypted fields specified in 'sampleEncryptedFields'.
function positiveUpdateTests(coll, invert = false) {
    function assertExpectedResult(result) {
        if (invert) {
            assert.commandFailedWithCode(result, ErrorCodes.DocumentValidationFailure);
        } else {
            assert.commandWorked(result);
        }
    }

    // first, insert a valid document to update
    assert.commandWorked(coll.insert({
        test_id: 0,
        firstName: validEncryptedString,
        a: {
            b: {
                c: validEncryptedInt,
                d: validEncryptedInt,
            },
            x: {
                y: validEncryptedString,
            }
        }
    }));

    jsTestLog("test unset & set of encrypted field");
    assertExpectedResult(coll.update({"test_id": 0}, {$unset: {"firstName": ""}}));
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": validEncryptedString}}));
    assertExpectedResult(
        coll.update({"test_id": 0}, {$set: {"a": {"x": {"y": validEncryptedString}}}}));
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.x": {"y": validEncryptedString}}}));

    jsTestLog("test updating prefix of encrypted field with non-array value");
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.b": 1}}));
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.x": {"z": 42}}}));
}

jsTestLog("test implicit validator only");
dbTest.test.drop();
assert.commandWorked(dbTest.createCollection("test", {encryptedFields: sampleEncryptedFields}));
negativeTests(dbTest.test, false);
positiveTests(dbTest.test, false);

jsTestLog("test implicit validator with user validator containing query ops");
dbTest.test.drop();
assert.commandWorked(dbTest.createCollection(
    "test", {encryptedFields: sampleEncryptedFields, validator: userQueryOpSchema}));
negativeTests(dbTest.test, true);
positiveTests(dbTest.test, true);

jsTestLog("test implicit validator with user validator containing json schema");
dbTest.test.drop();
assert.commandWorked(dbTest.createCollection(
    "test", {encryptedFields: sampleEncryptedFields, validator: userJsonSchema}));
negativeTests(dbTest.test, true);
positiveTests(dbTest.test, true);

jsTestLog("test user validator rules conflicting with implicit rules");
dbTest.test.drop();
assert.commandWorked(dbTest.createCollection(
    "test", {encryptedFields: sampleEncryptedFields, validator: userJsonConflictSchema}));
negativeTests(dbTest.test, true);
positiveTests(dbTest.test, true, true);

jsTestLog("test malformed user validator on encrypted collection");
dbTest.test.drop();
assert.commandFailed(dbTest.createCollection(
    "test", {encryptedFields: sampleEncryptedFields, validator: userMalformedSchema}));

jsTestLog("test FLE1 schema validator on FLE2 collection");
dbTest.test.drop();

assert.commandFailedWithCode(
    dbTest.createCollection("test",
                            {encryptedFields: sampleEncryptedFields, validator: fle1Schema}),
    224);

jsTestLog("test collMod adding user validator on encrypted collection");
dbTest.test.drop();
assert.commandWorked(dbTest.createCollection("test", {encryptedFields: sampleEncryptedFields}));
assert.commandWorked(dbTest.runCommand({collMod: "test", validator: userQueryOpSchema}));
negativeTests(dbTest.test, true);
positiveTests(dbTest.test, true);

jsTestLog("test collMod adding FLE1 user validator on encrypted collection");
dbTest.test.drop();
assert.commandWorked(dbTest.createCollection("test", {encryptedFields: sampleEncryptedFields}));
assert.commandFailedWithCode(dbTest.runCommand({collMod: "test", validator: fle1Schema}),
                             ErrorCodes.QueryFeatureNotAllowed);

jsTestLog("test implicit validation with validationAction set to 'warn'");
dbTest.test.drop();
// TODO: SERVER-64383 this should be prohibited
assert.commandWorked(dbTest.createCollection(
    "test", {encryptedFields: sampleEncryptedFields, validationAction: "warn"}));
negativeTests(dbTest.test, false, true);
positiveTests(dbTest.test, true);

jsTestLog("test implicit validation works on updates");
dbTest.test.drop();
assert.commandWorked(dbTest.createCollection("test", {encryptedFields: sampleEncryptedFields}));
negativeUpdateTests(dbTest.test);
positiveUpdateTests(dbTest.test);
}());
