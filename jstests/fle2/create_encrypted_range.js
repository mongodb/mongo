// Verify create encrypted collection with range index works

/**
 * @tags: [
 * requires_fcv_70,
 * assumes_unsharded_collection
 * ]
 */
(function() {
'use strict';

let dbTest = db.getSiblingDB('create_range_encrypted_collection_db');

dbTest.basic.drop();

const sampleEncryptedFields = {
    "fields": [
        {
            "path": "firstName",
            "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            "bsonType": "int",
            "queries":
                {"queryType": "rangePreview", "sparsity": 1, min: NumberInt(1), max: NumberInt(2)}
        },
    ]
};

assert.commandWorked(dbTest.createCollection("basic", {encryptedFields: sampleEncryptedFields}));
}());
