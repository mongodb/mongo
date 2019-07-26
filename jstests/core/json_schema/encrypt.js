/**
 * Tests for handling of the JSON Schema 'encrypt' keyword.
 *
 * @tags: [
 *   requires_non_retryable_commands,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/assert_schema_match.js");

const coll = db.jstests_schema_encrypt;
const encryptedBinDataElement = BinData(6, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA");
const nonEncryptedBinDataElement = BinData(0, "AAAAAAAAAAAAAAAAAAAAAAAAAAAA");

// Only elements of type BinData with subtype '6' should match.
assertSchemaMatch(coll, {properties: {bin: {encrypt: {}}}}, {bin: encryptedBinDataElement}, true);
assertSchemaMatch(coll, {properties: {bin: {encrypt: {}}}}, {bin: {}}, false);
assertSchemaMatch(
    coll, {properties: {bin: {encrypt: {}}}}, {bin: nonEncryptedBinDataElement}, false);
// Nested in object.
assertSchemaMatch(coll,
                  {properties: {obj: {type: 'object', properties: {a: {encrypt: {}}}}}},
                  {obj: {a: encryptedBinDataElement}},
                  true);
assertSchemaMatch(coll,
                  {properties: {obj: {type: 'object', properties: {a: {encrypt: {}}}}}},
                  {obj: {a: {}}},
                  false);
assertSchemaMatch(coll,
                  {properties: {obj: {type: 'object', properties: {a: {encrypt: {}}}}}},
                  {obj: {a: nonEncryptedBinDataElement}},
                  false);

// Nested in array.
assertSchemaMatch(coll,
                  {properties: {arr: {type: 'array', items: {encrypt: {}}}}},
                  {arr: [encryptedBinDataElement, encryptedBinDataElement]},
                  true);
assertSchemaMatch(
    coll, {properties: {arr: {type: 'array', items: {encrypt: {}}}}}, {arr: [{}, {}]}, false);
assertSchemaMatch(coll,
                  {properties: {arr: {type: 'array', items: {encrypt: {}}}}},
                  {arr: [encryptedBinDataElement, nonEncryptedBinDataElement]},
                  false);

// If array is not specified, should not traverse array of encrypted BinData's.
assertSchemaMatch(coll,
                  {properties: {bin: {encrypt: {}}}},
                  {bin: [encryptedBinDataElement, encryptedBinDataElement]},
                  false);

// Encrypt alongside type/bsontype should fail to parse.
assert.commandFailedWithCode(
    coll.runCommand(
        {find: "coll", filter: {$jsonSchema: {properties: {bin: {encrypt: {}, type: 'object'}}}}}),
    ErrorCodes.FailedToParse);

assert.commandFailedWithCode(coll.runCommand({
    find: "coll",
    filter: {$jsonSchema: {properties: {bin: {encrypt: {}, bsonType: 'object'}}}}
}),
                             ErrorCodes.FailedToParse);
}());
