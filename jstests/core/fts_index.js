/**
 * Test that:
 * 1. Text indexes properly validate the index spec used to create them.
 * 2. Text indexes properly enforce a schema on the language_override field.
 * 3. Collections may have at most one text index.
 * 4. Text indexes properly handle large documents.
 * 5. Bad weights test cases.
 *
 * @tags: [
 *   # Cannot implicitly shard accessed collections because of collection existing when none
 *   # expected.
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # Has operations which may never complete in stepdown/kill/terminate transaction tests.
 *   operations_longer_than_stepdown_interval_in_txns,
 *   # Uses index building in background
 *   requires_background_index,
 *   requires_fcv_49,
 * ]
 */

(function() {
'use strict';

const indexName = "textIndex";

const collNamePrefix = 'fts_index1_';
let collCount = 0;

let coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
coll.getDB().createCollection(coll.getName());

//
// 1. Text indexes properly validate the index spec used to create them.
//

// Spec passes text-specific index validation.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: "text"}, {name: indexName, default_language: "spanish"}));
assert.eq(1,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);

// Spec fails text-specific index validation ("spanglish" unrecognized).
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailed(
    coll.createIndex({a: "text"}, {name: indexName, default_language: "spanglish"}));
assert.eq(0,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);

// Spec passes general index validation.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({"$**": "text"}, {name: indexName}));
assert.eq(1,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);

// Spec fails general index validation ("a.$**" invalid field name for key).
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailed(coll.createIndex({"a.$**": "text"}, {name: indexName}));
assert.eq(0,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);

// SERVER-19519 Spec fails if '_fts' is specified on a non-text index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailed(coll.createIndex({_fts: 1}, {name: indexName}));
assert.eq(0,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailed(coll.createIndex({_fts: "text"}, {name: indexName}));
assert.eq(0,
          coll.getIndexes()
              .filter(function(z) {
                  return z.name == indexName;
              })
              .length);

//
// 2. Text indexes properly enforce a schema on the language_override field.
//

// Can create a text index on a collection where no documents have invalid language_override.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.insert({a: ""}));
assert.commandWorked(coll.insert({a: "", language: "spanish"}));
assert.commandWorked(coll.createIndex({a: "text"}));

// Can't create a text index on a collection containing document with an invalid language_override.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.insert({a: "", language: "spanglish"}));
assert.commandFailed(coll.createIndex({a: "text"}));

// Can insert documents with valid language_override into text-indexed collection.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: "text"}));
assert.commandWorked(coll.insert({a: ""}));
assert.commandWorked(coll.insert({a: "", language: "spanish"}));

// Can't insert documents with invalid language_override into text-indexed collection.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: "text"}));
assert.writeError(coll.insert({a: "", language: "spanglish"}));

//
// 3. Collections may have at most one text index.
//

// createIndex() becomes a no-op on an equivalent index spec.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}, {background: true}));
assert.eq(2, coll.getIndexes().length);
assert.commandFailedWithCode(coll.createIndex({a: 1, b: 1, c: "text"}),
                             ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(
    coll.createIndex({a: 1, _fts: "text", _ftsx: 1, c: 1}, {weights: {b: 1}}),
    ErrorCodes.IndexOptionsConflict);
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}, {default_language: "english"}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}, {textIndexVersion: 2}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}, {language_override: "language"}));
assert.eq(2, coll.getIndexes().length);

// Two index specs are also considered equivalent if they differ only in 'textIndexVersion', and
// createIndex() becomes a no-op on repeated requests that only differ in this way.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 2}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 3}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}));
assert.eq(2, coll.getIndexes().length);

coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 3}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 2}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}, {textIndexVersion: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: "text"}));
assert.eq(2, coll.getIndexes().length);

// createIndex() fails if a second text index would be built.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.getDB().createCollection(coll.getName()));
assert.eq(1, coll.getIndexes().length);
assert.commandWorked(coll.createIndex({a: 1, b: "text", c: 1}));
assert.eq(2, coll.getIndexes().length);
assert.commandFailed(coll.createIndex({a: 1, _fts: "text", _ftsx: 1, c: 1}, {weights: {d: 1}}));
assert.commandFailed(coll.createIndex({a: 1, b: "text", c: 1}, {default_language: "none"}));
assert.commandFailed(coll.createIndex({a: 1, b: "text", c: 1}, {language_override: "idioma"}));
assert.commandFailed(coll.createIndex({a: 1, b: "text", c: 1}, {weights: {d: 1}}));
assert.commandFailed(coll.createIndex({a: 1, b: "text", d: 1}));
assert.commandFailed(coll.createIndex({a: 1, d: "text", c: 1}));
assert.commandFailed(coll.createIndex({b: "text"}));
assert.commandFailed(coll.createIndex({b: "text", c: 1}));
assert.commandFailed(coll.createIndex({a: 1, b: "text"}));

//
// 4. Text indexes properly handle large keys.
//

coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.createIndex({a: "text"}));

let longstring = "";
let longstring2 = "";
for (let i = 0; i < 1024 * 1024; ++i) {
    longstring = longstring + "a";
    longstring2 = longstring2 + "b";
}
assert.commandWorked(coll.insert({_id: 0, a: longstring}));
assert.commandWorked(coll.insert({_id: 1, a: longstring2}));
assert.eq(1, coll.find({$text: {$search: longstring}}).itcount(), "long string not found in index");

//
// 5. Bad weights test cases.
//
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailed(coll.createIndex({a: 1, _fts: "text", _ftsx: 1, c: 1}, {weights: {}}));
assert.commandFailed(coll.createIndex({a: 1, _fts: "text", _ftsx: 1, c: 1}));

// The 'weights' parameter should only be allowed when the index is a text index.
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1, c: 1}, {weights: {d: 1}}),
                             ErrorCodes.CannotCreateIndex);
jsTestLog('indexes ' + coll.getFullName() + ':' + tojson(coll.getIndexes()));
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1, c: 1}, {weights: "$**"}),
                             ErrorCodes.CannotCreateIndex);
jsTestLog('indexes ' + coll.getFullName() + ':' + tojson(coll.getIndexes()));
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1, c: 1}, {weights: {}}),
                             ErrorCodes.CannotCreateIndex);
jsTestLog('indexes ' + coll.getFullName() + ':' + tojson(coll.getIndexes()));
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: 1, c: 1}, {weights: "$foo"}),
                             ErrorCodes.CannotCreateIndex);
jsTestLog('indexes ' + coll.getFullName() + ':' + tojson(coll.getIndexes()));
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 'abc', b: 100}));
assert.commandFailedWithCode(coll.createIndex({a: 'text'}, {weights: {b: Number.MAX_VALUE}}),
                             ErrorCodes.CannotCreateIndex);
jsTestLog('indexes ' + coll.getFullName() + ':' + tojson(coll.getIndexes()));
assert.commandFailedWithCode(coll.createIndex({a: 'text'}, {weights: {b: -Number.MAX_VALUE}}),
                             ErrorCodes.CannotCreateIndex);
jsTestLog('indexes ' + coll.getFullName() + ':' + tojson(coll.getIndexes()));

//
// 6. Bad direction value for non-text key in compound index.
//
coll = db.getCollection(collNamePrefix + collCount++);
coll.drop();
assert.commandFailedWithCode(coll.createIndex({a: "text", b: Number.MAX_VALUE}),
                             ErrorCodes.CannotCreateIndex);
assert.commandFailedWithCode(coll.createIndex({a: "text", b: -Number.MAX_VALUE}),
                             ErrorCodes.CannotCreateIndex);
jsTestLog('indexes ' + coll.getFullName() + ':' + tojson(coll.getIndexes()));
})();
