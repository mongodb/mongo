// Verify renaming to/from a collection with encrypted fields is disallowed

/**
 * @tags: [
 * requires_fcv_60,
 * assumes_unsharded_collection,
 * ]
 */
load("jstests/fle2/libs/encrypted_client_util.js");

(function() {
'use strict';

const srcDbName = 'rename_encrypted_collection_src_db';
const tgtDbName = 'rename_encrypted_collection_tgt_db';
const dbSrc = db.getSiblingDB(srcDbName);
const dbTgt = db.getSiblingDB(tgtDbName);

dbSrc.encrypted.drop();
dbSrc.unencrypted.drop();
dbSrc.renamed.drop();
dbTgt.encrypted.drop();
dbTgt.unencrypted.drop();

const sampleEncryptedFields = {
    "fields": [
        {
            "path": "firstName",
            "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            "bsonType": "string",
            "queries": {"queryType": "equality"}
        },
    ]
};
const srcEncryptedErrmsg = "Cannot rename an encrypted collection";
const tgtEncryptedErrmsg = "Cannot rename to an existing encrypted collection";

assert.commandWorked(dbSrc.createCollection("encrypted", {encryptedFields: sampleEncryptedFields}));
assert.commandWorked(dbSrc.createCollection("unencrypted"));

assert.commandWorked(dbTgt.createCollection("encrypted", {encryptedFields: sampleEncryptedFields}));

jsTestLog("Test renaming encrypted collection to another namespace is prohibited");
let res = assert.commandFailedWithCode(
    dbSrc.adminCommand({renameCollection: dbSrc + ".encrypted", to: dbSrc + ".renamed"}),
    ErrorCodes.IllegalOperation,
    "Renaming an encrypted collection within same DB passed");
assert.eq(res.errmsg, srcEncryptedErrmsg);

res = assert.commandFailedWithCode(
    dbSrc.adminCommand({renameCollection: dbSrc + ".encrypted", to: dbTgt + ".unencrypted"}),
    ErrorCodes.IllegalOperation,
    "Renaming an encrypted collection between DBs passed");
assert.eq(res.errmsg, srcEncryptedErrmsg);

jsTestLog("Test renaming unencrypted collection to an encrypted namespace is prohibited");
res = assert.commandFailedWithCode(
    dbSrc.adminCommand(
        {renameCollection: dbSrc + ".unencrypted", to: dbSrc + ".encrypted", dropTarget: true}),
    ErrorCodes.IllegalOperation,
    "Renaming to an encrypted collection within same DB passed");
assert.eq(res.errmsg, tgtEncryptedErrmsg);

res = assert.commandFailedWithCode(
    dbSrc.adminCommand(
        {renameCollection: dbSrc + ".unencrypted", to: dbTgt + ".encrypted", dropTarget: true}),
    ErrorCodes.IllegalOperation,
    "Renaming to an encrypted collection between DBs passed");
assert.eq(res.errmsg, tgtEncryptedErrmsg);
}());
