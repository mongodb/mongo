// Verify implicit schema validation works for encrypted collections

/**
 * @tags: [
 * assumes_unsharded_collection,
 * does_not_support_transactions
 * ]
 */
(function() {
'use strict';
load("jstests/libs/doc_validation_utils.js");  // for assertDocumentValidationFailure

const dbTest = db.getSiblingDB('implicit_schema_validation_db');

const validEncryptedString = HexData(6, "060102030405060708091011121314151602");
const validEncryptedInt = HexData(6, "060102030405060708091011121314151610");
const nonEncryptedBinData = HexData(3, "060102030405060708091011121314151610");
const fle1RandomBinData = HexData(6, "020102030405060708091011121314151602");
const fle2PlaceholderBinData = HexData(6, "030102030405060708091011121314151602");

const typeMatchedArrayError = {
    operator: "type",
    reason: "type did match",
    consideredType: "array"
};
const valueNotEncryptedError = {
    operator: "fle2Encrypt",
    reason: "value was not encrypted"
};
const wrongEncryptedTypeError = {
    operator: "fle2Encrypt",
    reason: "Queryable Encryption encrypted value has wrong type"
};

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

/**
 * Finds the sub-object starting at 'obj' that contains the property 'key'
 * and has the string value 'value'
 * @param {object} obj the object to traverse
 * @param {string} key the attribute name to find
 * @param {string} value the attribute value to find
 * @returns the first subobject found that contains the target key-value pair
 */
function findContainingObject(obj, key, value) {
    let queue = [obj];
    while (queue.length > 0) {
        let o = queue.shift();
        if (o.hasOwnProperty(key) && o[key] === value) {
            return o;
        }
        for (let prop in o) {
            if (typeof o[prop] === "object" && o[prop] !== null) {
                queue.push(o[prop]);
            }
        }
    }
    return null;
}

/**
 * Asserts the result of a command is a document validation failure.
 * If 'fleErrors' is defined, then this asserts that the errInfo in the result
 * contains an "implicitFLESchema" annotation, and an annotation for each
 * attribute in 'fleErrors'. Each attribute in 'fleErrors' is a pair where the key
 * is the encrypted field path that is expected to cause an error, and the value is
 * an object containing the expected 'operatorName' and detail fields.
 */
function assertFailedWithAnnotation(result, coll, fleErrors) {
    assertDocumentValidationFailure(result, coll);
    assert(result instanceof WriteResult);
    const errInfo = result.getWriteError().errInfo;
    const schema = findContainingObject(errInfo, "operatorName", "implicitFLESchema");

    if (fleErrors) {
        assert(schema,
               "Result errInfo does not contain an implicitFLESchema error: " + tojson(errInfo));
    } else {
        assert(!schema,
               "Result errInfo contains unexpected implicitFLESchema error: " + tojson(errInfo));
        return;
    }
    assert(schema.hasOwnProperty("schemaRulesNotSatisfied"));

    for (let path in fleErrors) {
        const pathParts = path.split('.');
        let subschema = schema;
        for (let pathIdx in pathParts) {
            subschema = findContainingObject(subschema, "propertyName", pathParts[pathIdx]);
            assert(subschema, "No errors found for property '" + path + "': " + tojson(errInfo));
        }
        assert(subschema.hasOwnProperty("details"),
               "No error details found for property '" + path + "': " + tojson(errInfo));

        const detail =
            findContainingObject(subschema.details, "operatorName", fleErrors[path].operator);
        assert(detail,
               "Error details for property '" + path +
                   "' does not contain the expected operator '" + fleErrors[path].operator +
                   "': " + tojson(errInfo));

        for (let field in fleErrors[path]) {
            if (field === "operator") {
                continue;
            }
            const detailWithField = findContainingObject(detail, field, fleErrors[path][field]);
            assert(detailWithField,
                   "Error details for property '" + path + "' does not contain the expected " +
                       field + " '" + fleErrors[path][field] + "': " + tojson(errInfo));
        }
    }
}

// Tests invalid inserts on encrypted collection 'coll'.
// This assumes 'coll' was created encrypted fields specified in 'sampleEncryptedFields'.
// If 'hasUserValidator' is true, this assumes it validates the optional field 'name' is a string.
function negativeTests(coll, hasUserValidator, invert = false) {
    function assertExpectedResult(result, fleErrors) {
        if (invert) {
            assert.commandWorked(result);
        } else {
            assertFailedWithAnnotation(result, coll, fleErrors);
        }
        return result;
    }

    jsTestLog("test inserting non-bindata value for encrypted field");
    assertExpectedResult(coll.insert({firstName: "foo"}), {firstName: valueNotEncryptedError});
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: "bar",
                d: "foo",
            },
        }
    }),
                         {"a.b.c": valueNotEncryptedError, "a.b.d": valueNotEncryptedError});

    jsTestLog("test path to encrypted field has arrays");
    assertExpectedResult(coll.insert({a: [{b: {c: validEncryptedInt}}]}),
                         {"a": typeMatchedArrayError});
    assertExpectedResult(coll.insert({a: {b: [{c: validEncryptedInt}]}}),
                         {"a.b": typeMatchedArrayError});
    assertExpectedResult(coll.insert({a: {b: {c: []}}}), {"a.b.c": valueNotEncryptedError});

    jsTestLog("test inserting encrypted field with BinData of incorrect subtype");
    assertExpectedResult(coll.insert({firstName: nonEncryptedBinData}),
                         {firstName: valueNotEncryptedError});
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: nonEncryptedBinData,
                d: validEncryptedInt,
            },
        }
    }),
                         {"a.b.c": valueNotEncryptedError});

    jsTestLog("test inserting encrypted field with incorrect Queryable Encryption subtype");
    assertExpectedResult(coll.insert({firstName: fle1RandomBinData}),
                         {firstName: wrongEncryptedTypeError});
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: fle2PlaceholderBinData,
                d: validEncryptedInt,
            },
        }
    }),
                         {"a.b.c": wrongEncryptedTypeError});

    jsTestLog(
        "test inserting encrypted field with incorrect BSONType specifier for the unencrypted value");
    assertExpectedResult(coll.insert({firstName: validEncryptedInt}),
                         {firstName: wrongEncryptedTypeError});
    assertExpectedResult(coll.insert({
        firstName: validEncryptedString,
        a: {
            b: {
                c: validEncryptedString,
                d: validEncryptedInt,
            },
        }
    }),
                         {"a.b.c": wrongEncryptedTypeError});

    if (!hasUserValidator) {
        return;
    }

    jsTestLog("test insert violating user-provided validator");
    assertExpectedResult(coll.insert({firstName: validEncryptedString, name: 234}));
    assertExpectedResult(coll.insert({firstName: nonEncryptedBinData, name: 234}),
                         {firstName: valueNotEncryptedError});
}

// Tests invalid updates on encrypted collection 'coll'
// This assumes 'coll' was created encrypted fields specified in 'sampleEncryptedFields'.
function negativeUpdateTests(coll, invert = false) {
    function assertExpectedResult(result, fleErrors) {
        if (invert) {
            assert.commandWorked(result);
        } else {
            assertFailedWithAnnotation(result, coll, fleErrors);
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
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": "roger"}}),
                         {firstName: valueNotEncryptedError});
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": nonEncryptedBinData}}),
                         {firstName: valueNotEncryptedError});
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": fle1RandomBinData}}),
                         {firstName: wrongEncryptedTypeError});
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"firstName": validEncryptedInt}}),
                         {firstName: wrongEncryptedTypeError});
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.x.y": [1, 2, 3]}}),
                         {"a.x.y": valueNotEncryptedError});
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.x": {"y": 42}}}),
                         {"a.x": valueNotEncryptedError});

    jsTestLog("test updating prefix of encrypted field with array value");
    assertExpectedResult(coll.update({"test_id": 0}, {$set: {"a.b": [1, 2, 3]}}),
                         {"a.b": typeMatchedArrayError});
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

jsTestLog("test FLE1 schema validator on Queryable Encryption collection");
dbTest.test.drop();
assert.commandFailedWithCode(
    dbTest.createCollection("test",
                            {encryptedFields: sampleEncryptedFields, validator: fle1Schema}),
    ErrorCodes.QueryFeatureNotAllowed);

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

jsTestLog("test implicit validation works on updates");
dbTest.test.drop();
assert.commandWorked(dbTest.createCollection("test", {encryptedFields: sampleEncryptedFields}));
negativeUpdateTests(dbTest.test);
positiveUpdateTests(dbTest.test);
}());
