// Verify valid and invalid scenarios for create encrypted collection

/**
 * @tags: [
 *  featureFlagFLE2,
 * ]
 */
load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
'use strict';

if (!isFLE2Enabled()) {
    return;
}

let dbTest = db.getSiblingDB('create_encrypted_collection_db');

dbTest.basic.drop();

const sampleEncryptedFields = {
    "fields": [
        {
            "path": "firstName",
            "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            "bsonType": "string",
            "queries": {"queryType": "equality"}  // allow single object or array
        },
    ]
};

assert.commandFailedWithCode(
    dbTest.createCollection("basic", {viewOn: "foo", encryptedFields: sampleEncryptedFields}),
    6346401,
    "Create with encryptedFields and viewOn passed");

assert.commandFailedWithCode(
    dbTest.createCollection(
        "basic", {timeseries: {timeField: 'time'}, encryptedFields: sampleEncryptedFields}),
    6346401,
    "Create with encryptedFields and timeseries passed");

assert.commandFailedWithCode(
    dbTest.createCollection("basic",
                            {capped: true, size: 100000, encryptedFields: sampleEncryptedFields}),
    6367301,
    "Create with encryptedFields and capped passed");

assert.commandFailedWithCode(
    dbTest.createCollection("basic",
                            {validationAction: "warn", encryptedFields: sampleEncryptedFields}),
    ErrorCodes.BadValue);

assert.commandFailedWithCode(
    dbTest.createCollection("basic",
                            {encryptedFields: sampleEncryptedFields, validationLevel: "off"}),
    ErrorCodes.BadValue);

assert.commandFailedWithCode(
    dbTest.createCollection("basic",
                            {encryptedFields: sampleEncryptedFields, validationLevel: "moderate"}),
    ErrorCodes.BadValue);

assert.commandWorked(dbTest.createCollection("basic", {encryptedFields: sampleEncryptedFields}));

const result = dbTest.getCollectionInfos({name: "basic"});
const ef = result[0].options.encryptedFields;
assert.eq(ef.escCollection, "fle2.basic.esc");
assert.eq(ef.eccCollection, "fle2.basic.ecc");
assert.eq(ef.ecocCollection, "fle2.basic.ecoc");
}());
