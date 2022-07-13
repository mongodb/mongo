/**
 * @tags: [
 *   assumes_superuser_permissions,
 * ]
 * fcv49 for the change to error code in createIndexes invalid field reply.
 */
(function() {
'use strict';

const kUnknownIDLFieldError = 40415;
const isMongos = ("isdbgrid" == db.runCommand("hello").msg);

const extractResult = function(obj) {
    if (!isMongos)
        return obj;

    // Sample mongos format:
    // {
    //   raw: {
    //     "localhost:30000": {
    //       createdCollectionAutomatically: false,
    //       numIndexesBefore: 3,
    //       numIndexesAfter: 5,
    //       ok: 1
    //     }
    //   },
    //   ok: 1
    // }

    let numFields = 0;
    let result = null;
    for (let field in obj.raw) {
        result = obj.raw[field];
        numFields++;
    }

    assert.neq(null, result);
    assert.eq(1, numFields);
    return result;
};

const checkImplicitCreate = function(createIndexResult) {
    assert.eq(true, createIndexResult.createdCollectionAutomatically);
};

const assertIndexes = function(coll, expectedIndexNames) {
    const indexSpecs = coll.getIndexes();
    const indexNames = indexSpecs.map(spec => spec.name);
    assert.sameMembers(indexNames, expectedIndexNames, tojson(indexSpecs));
};

const dbTest = db.getSiblingDB('create_indexes_db');
dbTest.dropDatabase();

// Database does not exist
const collDbNotExist = dbTest.create_indexes_no_db;
let res = assert.commandWorked(
    collDbNotExist.runCommand('createIndexes', {indexes: [{key: {x: 1}, name: 'x_1'}]}));
res = extractResult(res);
checkImplicitCreate(res);
assert.eq(res.numIndexesAfter, res.numIndexesBefore + 1);
assert.isnull(
    res.note,
    'createIndexes.note should not be present in results when adding a new index: ' + tojson(res));

// Collection does not exist, but database does
const t = dbTest.create_indexes;
res = assert.commandWorked(t.runCommand('createIndexes', {indexes: [{key: {x: 1}, name: 'x_1'}]}));
res = extractResult(res);
checkImplicitCreate(res);
assert.eq(res.numIndexesAfter, res.numIndexesBefore + 1);
assert.isnull(
    res.note,
    'createIndexes.note should not be present in results when adding a new index: ' + tojson(res));

// Both database and collection exist
res = assert.commandWorked(t.runCommand('createIndexes', {indexes: [{key: {x: 1}, name: 'x_1'}]}));
res = extractResult(res);
assert(!res.createdCollectionAutomatically);
assert.eq(res.numIndexesBefore,
          res.numIndexesAfter,
          'numIndexesAfter missing from createIndexes result when adding a duplicate index: ' +
              tojson(res));
assert(res.note,
       'createIndexes.note should be present in results when adding a duplicate index: ' +
           tojson(res));

res = t.runCommand("createIndexes",
                   {indexes: [{key: {"x": 1}, name: "x_1"}, {key: {"y": 1}, name: "y_1"}]});
res = extractResult(res);
assert(!res.createdCollectionAutomatically);
assert.eq(res.numIndexesAfter, res.numIndexesBefore + 1);

res = assert.commandWorked(t.runCommand(
    'createIndexes', {indexes: [{key: {a: 1}, name: 'a_1'}, {key: {b: 1}, name: 'b_1'}]}));
res = extractResult(res);
assert(!res.createdCollectionAutomatically);
assert.eq(res.numIndexesAfter, res.numIndexesBefore + 2);
assert.isnull(
    res.note,
    'createIndexes.note should not be present in results when adding new indexes: ' + tojson(res));

res = assert.commandWorked(t.runCommand(
    'createIndexes', {indexes: [{key: {a: 1}, name: 'a_1'}, {key: {b: 1}, name: 'b_1'}]}));

res = extractResult(res);
assert.eq(res.numIndexesBefore,
          res.numIndexesAfter,
          'numIndexesAfter missing from createIndexes result when adding duplicate indexes: ' +
              tojson(res));
assert(res.note,
       'createIndexes.note should be present in results when adding a duplicate index: ' +
           tojson(res));

// Test that index creation fails with an empty list of specs.
res = t.runCommand("createIndexes", {indexes: []});
assert.commandFailedWithCode(res, ErrorCodes.BadValue);

// Test that index creation fails on specs that are missing required fields such as 'key'.
res = t.runCommand("createIndexes", {indexes: [{}]});
assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

// Test that any malformed specs in the list causes the entire index creation to fail and
// will not result in new indexes in the catalog.
res = t.runCommand("createIndexes", {indexes: [{}, {key: {m: 1}, name: "asd"}]});
assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);
assertIndexes(t, ["_id_", "x_1", "y_1", "a_1", "b_1"]);

res = t.runCommand("createIndexes", {indexes: [{key: {"c": 1}, sparse: true, name: "c_1"}]});
assertIndexes(t, ["_id_", "x_1", "y_1", "a_1", "b_1", "c_1"]);
assert.eq(1,
          t.getIndexes()
              .filter(function(z) {
                  return z.sparse;
              })
              .length);

// Test that index creation fails if we specify an unsupported index type.
res = t.runCommand("createIndexes", {indexes: [{key: {"x": "invalid_index_type"}, name: "x_1"}]});
assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);

assertIndexes(t, ["_id_", "x_1", "y_1", "a_1", "b_1", "c_1"]);

// Test that an index name, if provided by the user, cannot be empty.
res = t.runCommand("createIndexes", {indexes: [{key: {"x": 1}, name: ""}]});
assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);

assertIndexes(t, ["_id_", "x_1", "y_1", "a_1", "b_1", "c_1"]);

// Test that v0 indexes cannot be created.
res = t.runCommand('createIndexes', {indexes: [{key: {d: 1}, name: 'd_1', v: 0}]});
assert.commandFailed(res, 'v0 index creation should fail');

// Test that v1 indexes can be created explicitly.
res = t.runCommand('createIndexes', {indexes: [{key: {d: 1}, name: 'd_1', v: 1}]});
assert.commandWorked(res, 'v1 index creation should succeed');

// Test that index creation fails with an invalid top-level field.
res = t.runCommand('createIndexes', {indexes: [{key: {e: 1}, name: 'e_1'}], 'invalidField': 1});
assert.commandFailedWithCode(res, kUnknownIDLFieldError);

// Test that index creation fails with an invalid field in the index spec for index version V2.
res = t.runCommand('createIndexes',
                   {indexes: [{key: {e: 1}, name: 'e_1', 'v': 2, 'invalidField': 1}]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidIndexSpecificationOption);

// Test that index creation fails with an invalid field in the index spec for index version V1.
res = t.runCommand('createIndexes',
                   {indexes: [{key: {e: 1}, name: 'e_1', 'v': 1, 'invalidField': 1}]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidIndexSpecificationOption);

// Test that index creation fails with an index named '*'.
res = t.runCommand('createIndexes', {indexes: [{key: {star: 1}, name: '*'}]});
assert.commandFailedWithCode(res, ErrorCodes.BadValue);

// Test that index creation fails with an index value of empty string.
res = t.runCommand('createIndexes', {indexes: [{key: {f: ""}, name: 'f_1'}]});
assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);

// Test that index creation fails with duplicate index names in the index specs.
res = t.runCommand('createIndexes', {
    indexes: [
        {key: {g: 1}, name: 'myidx'},
        {key: {h: 1}, name: 'myidx'},
    ],
});
assert.commandFailedWithCode(res, ErrorCodes.IndexKeySpecsConflict);

// Test that user is not allowed to create indexes in config.transactions.
const configDB = db.getSiblingDB('config');
res =
    configDB.runCommand({createIndexes: 'transactions', indexes: [{key: {star: 1}, name: 'star'}]});
assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);

// Test that providing an empty list of index spec for config.transactions should also fail with
// IllegalOperation, rather than BadValue for a normal collection.
// This is consistent with server behavior prior to 6.0.
res = configDB.runCommand({createIndexes: 'transactions', indexes: []});
assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
}());
