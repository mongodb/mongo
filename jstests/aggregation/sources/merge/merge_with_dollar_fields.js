// Tests $merge over documents with $-field in it.
//
// Sharded collections have special requirements on the join field.
// @tags: [assumes_unsharded_collection]

(function() {
"use strict";

load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection.

const sourceName = 'merge_with_dollar_fields_source';
const source = db[sourceName];
const targetName = 'merge_with_dollar_fields_target';
const target = db[targetName];

const joinField = 'joinField';
const sourceDoc = {
    $dollar: 1,
    joinField
};
const targetDoc = {
    a: 1,
    joinField
};
assertDropCollection(db, sourceName);
assert.commandWorked(source.insert(sourceDoc));

function runTest({whenMatched, whenNotMatched}, targetDocs) {
    assertDropCollection(db, targetName);
    assert.commandWorked(target.createIndex({joinField: 1}, {unique: true}));
    assert.commandWorked(target.insert(targetDocs));
    source.aggregate([
        {$project: {_id: 0}},
        {
            $merge: {
                into: targetName,
                on: joinField,
                whenMatched,
                whenNotMatched,
            }
        }
    ]);
    return target.findOne({}, {_id: 0});
}

function runTestMatched(mode) {
    return runTest(mode, [targetDoc]);
}

function runTestNotMatched(mode) {
    return runTest(mode, []);
}

// TODO: SERVER-76999: Currently $merge may throw 'FailedToParse' error due to non-local updates.
// We should return consistent results for dollar field documents.

// whenMatched: 'replace', whenNotMatched: 'insert'
assert.throwsWithCode(() => runTestMatched({whenMatched: 'replace', whenNotMatched: 'insert'}),
                      [ErrorCodes.DollarPrefixedFieldName, ErrorCodes.FailedToParse]);

try {
    assert.docEq(sourceDoc, runTestNotMatched({whenMatched: 'replace', whenNotMatched: 'insert'}));
} catch (error) {
    assert.commandFailedWithCode(error, ErrorCodes.FailedToParse);
}

// whenMatched: 'replace', whenNotMatched: 'fail'
assert.throwsWithCode(() => runTestMatched({whenMatched: 'replace', whenNotMatched: 'fail'}),
                      [ErrorCodes.DollarPrefixedFieldName, ErrorCodes.FailedToParse]);

assert.throwsWithCode(() => runTestNotMatched({whenMatched: 'replace', whenNotMatched: 'fail'}),
                      [ErrorCodes.MergeStageNoMatchingDocument, ErrorCodes.FailedToParse]);

// whenMatched: 'replace', whenNotMatched: 'discard'
assert.throwsWithCode(() => runTestMatched({whenMatched: 'replace', whenNotMatched: 'discard'}),
                      [ErrorCodes.DollarPrefixedFieldName, ErrorCodes.FailedToParse]);

try {
    assert.eq(null, runTestNotMatched({whenMatched: 'replace', whenNotMatched: 'discard'}));
} catch (error) {
    assert.commandFailedWithCode(error, ErrorCodes.FailedToParse);
}

// whenMatched: 'merge', whenNotMatched: 'insert'
assert.throwsWithCode(() => runTestMatched({whenMatched: 'merge', whenNotMatched: 'insert'}),
                      ErrorCodes.DollarPrefixedFieldName);

assert.docEq(sourceDoc, runTestNotMatched({whenMatched: 'merge', whenNotMatched: 'insert'}));

// whenMatched: 'merge', whenNotMatched: 'fail'
assert.throwsWithCode(() => runTestMatched({whenMatched: 'merge', whenNotMatched: 'fail'}),
                      ErrorCodes.DollarPrefixedFieldName);

assert.throwsWithCode(() => runTestNotMatched({whenMatched: 'merge', whenNotMatched: 'fail'}),
                      ErrorCodes.MergeStageNoMatchingDocument);

// whenMatched: 'merge', whenNotMatched: 'discard'
assert.throwsWithCode(() => runTestMatched({whenMatched: 'merge', whenNotMatched: 'discard'}),
                      ErrorCodes.DollarPrefixedFieldName);

assert.eq(null, runTestNotMatched({whenMatched: 'merge', whenNotMatched: 'discard'}));

// whenMatched: 'keepExisting', whenNotMatched: 'insert'
assert.docEq(targetDoc, runTestMatched({whenMatched: 'keepExisting', whenNotMatched: 'insert'}));

assert.docEq(sourceDoc, runTestNotMatched({whenMatched: 'keepExisting', whenNotMatched: 'insert'}));

// whenMatched: 'fail', whenNotMatched: 'insert'
assert.throwsWithCode(() => runTestMatched({whenMatched: 'fail', whenNotMatched: 'insert'}),
                      ErrorCodes.DuplicateKey);

assert.docEq(sourceDoc, runTestNotMatched({whenMatched: 'fail', whenNotMatched: 'insert'}));

// whenMatched: 'pipeline', whenNotMatched: 'insert'
const pipeline = [{$addFields: {b: 1}}];
const targetDocAddFields = Object.assign({}, targetDoc, {b: 1});
assert.docEq(targetDocAddFields, runTestMatched({whenMatched: pipeline, whenNotMatched: 'insert'}));

assert.docEq(sourceDoc, runTestNotMatched({whenMatched: pipeline, whenNotMatched: 'insert'}));

// whenMatched: 'pipeline', whenNotMatched: 'fail'
assert.docEq(targetDocAddFields, runTestMatched({whenMatched: pipeline, whenNotMatched: 'fail'}));

assert.throwsWithCode(() => runTestNotMatched({whenMatched: pipeline, whenNotMatched: 'fail'}),
                      ErrorCodes.MergeStageNoMatchingDocument);

// whenMatched: 'pipeline', whenNotMatched: 'discard'
assert.docEq(targetDocAddFields,
             runTestMatched({whenMatched: pipeline, whenNotMatched: 'discard'}));

assert.eq(null, runTestNotMatched({whenMatched: pipeline, whenNotMatched: 'discard'}));
}());
