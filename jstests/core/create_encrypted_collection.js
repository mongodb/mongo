// Verify valid and invalid scenarios for create encrypted collection

/**
 * @tags: [
 *  featureFlagFLE2,
 *  assumes_against_mongod_not_mongos
 * ]
 */
(function() {
'use strict';

const isFLE2Enabled = TestData == undefined || TestData.setParameters.featureFlagFLE2;

if (!isFLE2Enabled) {
    return;
}

let dbTest = db.getSiblingDB('create_encrypted_collection_db');

dbTest.basic.drop();

assert.commandWorked(dbTest.createCollection("basic", {
    encryptedFields: {
        "fields": [
            {
                "path": "firstName",
                "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
                "bsonType": "string",
                "queries": {"queryType": "equality"}  // allow single object or array
            },

        ]
    }
}));

const result = dbTest.getCollectionInfos({name: "basic"});
print("result" + tojson(result));
const ef = result[0].options.encryptedFields;
assert.eq(ef.escCollection, "fle2.basic.esc");
assert.eq(ef.eccCollection, "fle2.basic.ecc");
assert.eq(ef.ecocCollection, "fle2.basic.ecoc");
}());
