// Verify encryption-related keywords are only allowed in document validators if the action is
// 'error' and validation level is 'strict'.
//
// Cannot implicitly shard accessed collections because of collection existing when none
// expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop, requires_non_retryable_commands,
// requires_fcv_47]
(function() {
"use strict";

var collName = "doc_validation_encrypt_keywords";
var coll = db[collName];
coll.drop();

const encryptSchema = {
    $jsonSchema: {properties: {_id: {encrypt: {}}}}
};
const nestedEncryptSchema = {
    $jsonSchema: {properties: {user: {type: "object", properties: {ssn: {encrypt: {}}}}}}
};
const encryptMetadataSchema = {
    $jsonSchema: {encryptMetadata: {algorithm: "AEAD_AES_256_CBC_HMAC_SHA_512-Deterministic"}}
};

assert.commandFailedWithCode(
    db.createCollection(collName, {validator: encryptSchema, validationAction: "warn"}),
    ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(
    db.createCollection(collName, {validator: nestedEncryptSchema, validationAction: "warn"}),
    ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(
    db.createCollection(collName, {validator: encryptMetadataSchema, validationAction: "warn"}),
    ErrorCodes.QueryFeatureNotAllowed);

assert.commandFailedWithCode(
    db.createCollection(collName, {validator: encryptSchema, validationLevel: "moderate"}),
    ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(
    db.createCollection(collName, {validator: nestedEncryptSchema, validationLevel: "moderate"}),
    ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(
    db.createCollection(collName, {validator: encryptMetadataSchema, validationLevel: "moderate"}),
    ErrorCodes.QueryFeatureNotAllowed);

// Create the collection with a valid document validator and action 'warn'.
assert.commandWorked(db.createCollection(
    collName, {validator: {$jsonSchema: {required: ["_id"]}}, validationAction: "warn"}));

// Verify that we can't collMod the validator to include an encryption-related keyword.
assert.commandFailedWithCode(db.runCommand({collMod: collName, validator: encryptSchema}),
                             ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(db.runCommand({collMod: collName, validator: nestedEncryptSchema}),
                             ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(db.runCommand({collMod: collName, validator: encryptMetadataSchema}),
                             ErrorCodes.QueryFeatureNotAllowed);
coll.drop();

// Create the collection with an encrypted validator and action 'error'.
assert.commandWorked(
    db.createCollection(collName, {validator: encryptSchema, validationAction: "error"}));

// Verify that we can't collMod the validation action to 'warn' since the schema contains an
// encryption-related keyword.
assert.commandFailedWithCode(db.runCommand({collMod: collName, validationAction: "warn"}),
                             ErrorCodes.QueryFeatureNotAllowed);

// Verify that we can't collMod the validation level to 'moderate' since the schema contains an
// encryption-related keyword.
assert.commandFailedWithCode(db.runCommand({collMod: collName, validationLevel: "moderate"}),
                             ErrorCodes.QueryFeatureNotAllowed);
coll.drop();

// Create the collection without a document validator.
assert.commandWorked(db.createCollection(collName));

// Verify that we can't collMod with an encrypted validator and validation action 'warn' or level
// 'moderate'.
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, validator: encryptSchema, validationAction: "warn"}),
    ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, validator: nestedEncryptSchema, validationAction: "warn"}),
    ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, validator: encryptMetadataSchema, validationAction: "warn"}),
    ErrorCodes.QueryFeatureNotAllowed);

assert.commandFailedWithCode(
    db.runCommand({collMod: collName, validator: encryptSchema, validationLevel: "moderate"}),
    ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(
    db.runCommand({collMod: collName, validator: nestedEncryptSchema, validationLevel: "moderate"}),
    ErrorCodes.QueryFeatureNotAllowed);
assert.commandFailedWithCode(
    db.runCommand(
        {collMod: collName, validator: encryptMetadataSchema, validationLevel: "moderate"}),
    ErrorCodes.QueryFeatureNotAllowed);
coll.drop();
})();
