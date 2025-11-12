/**
 * @tags: [
 *   assumes_superuser_permissions,
 *   # simulate_atlas_proxy.js can't simulate req on config.transaction as tested
 *   simulate_atlas_proxy_incompatible,
 * ]
 */
import {IndexUtils} from "jstests/libs/index_utils.js";

const dbTest = db.getSiblingDB("create_indexes_db");
dbTest.dropDatabase();

const t = dbTest.create_indexes;
dbTest.createCollection(t.getName());

// Test that index creation fails with an empty list of specs.
let res = t.runCommand("createIndexes", {indexes: []});
assert.commandFailedWithCode(res, ErrorCodes.BadValue);

// Test that index creation fails on specs that are missing required fields such as 'key'.
res = t.runCommand("createIndexes", {indexes: [{}]});
assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

// Test that any malformed specs in the list causes the entire index creation to fail and
// will not result in new indexes in the catalog.
res = t.runCommand("createIndexes", {indexes: [{}, {key: {m: 1}, name: "asd"}]});
assert.commandFailedWithCode(res, ErrorCodes.FailedToParse);

IndexUtils.assertIndexes(t, [{_id: 1}]);

res = t.runCommand("createIndexes", {indexes: [{key: {"c": 1}, sparse: true, name: "c_1"}]});
IndexUtils.assertIndexes(t, [{_id: 1}, {c: 1}]);
assert.eq(
    1,
    t.getIndexes().filter(function (z) {
        return z.sparse;
    }).length,
);

// Test that index creation fails if we specify an unsupported index type.
res = t.runCommand("createIndexes", {indexes: [{key: {"x": "invalid_index_type"}, name: "x_1"}]});
assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);

IndexUtils.assertIndexes(t, [{_id: 1}, {c: 1}]);

// Test that an index name, if provided by the user, cannot be empty.
res = t.runCommand("createIndexes", {indexes: [{key: {"x": 1}, name: ""}]});
assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);

IndexUtils.assertIndexes(t, [{_id: 1}, {c: 1}]);

// Test that v0 indexes cannot be created.
res = t.runCommand("createIndexes", {indexes: [{key: {d: 1}, name: "d_1", v: 0}]});
assert.commandFailed(res, "v0 index creation should fail");

IndexUtils.assertIndexes(t, [{_id: 1}, {c: 1}]);

// Test that v1 indexes can be created explicitly.
res = t.runCommand("createIndexes", {indexes: [{key: {d: 1}, name: "d_1", v: 1}]});
assert.commandWorked(res, "v1 index creation should succeed");

IndexUtils.assertIndexes(t, [{_id: 1}, {c: 1}, {d: 1}]);

// Test that index creation fails with an invalid top-level field.
res = t.runCommand("createIndexes", {indexes: [{key: {e: 1}, name: "e_1"}], "invalidField": 1});
assert.commandFailedWithCode(res, ErrorCodes.IDLUnknownField);

// Test that index creation fails with an invalid field in the index spec for index version V2.
res = t.runCommand("createIndexes", {indexes: [{key: {e: 1}, name: "e_1", "v": 2, "invalidField": 1}]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidIndexSpecificationOption);

// Test that index creation fails with an invalid field in the index spec for index version V1.
res = t.runCommand("createIndexes", {indexes: [{key: {e: 1}, name: "e_1", "v": 1, "invalidField": 1}]});
assert.commandFailedWithCode(res, ErrorCodes.InvalidIndexSpecificationOption);

IndexUtils.assertIndexes(t, [{_id: 1}, {c: 1}, {d: 1}]);

// Test that index creation fails with an index named '*'.
res = t.runCommand("createIndexes", {indexes: [{key: {star: 1}, name: "*"}]});
assert.commandFailedWithCode(res, ErrorCodes.BadValue);

// Test that index creation fails with an index value of empty string.
res = t.runCommand("createIndexes", {indexes: [{key: {f: ""}, name: "f_1"}]});
assert.commandFailedWithCode(res, ErrorCodes.CannotCreateIndex);

// Test that index creation fails with duplicate index names in the index specs.
res = t.runCommand("createIndexes", {
    indexes: [
        {key: {g: 1}, name: "myidx"},
        {key: {h: 1}, name: "myidx"},
    ],
});
assert.commandFailedWithCode(res, ErrorCodes.IndexKeySpecsConflict);

IndexUtils.assertIndexes(t, [{_id: 1}, {c: 1}, {d: 1}]);

// Test that creating an index on a view fails with CollectionUUIDMismatch if a collection UUID is
// provided. CollectionUUIDMismatch has to prevail over CommandNotSupportedOnView for mongosync.
assert.commandWorked(db.createView("toApple", "apple", []));
res = db.runCommand({
    createIndexes: "toApple",
    collectionUUID: UUID(),
    indexes: [{name: "_id_hashed", key: {_id: "hashed"}}],
});
assert.commandFailedWithCode(res, ErrorCodes.CollectionUUIDMismatch);

// Test that creating an index on a view fails with CommandNotSupportedOnView if a collection UUID
// is not provided
assert.commandWorked(db.createView("toApple", "apple", []));
res = db.runCommand({createIndexes: "toApple", indexes: [{name: "_id_hashed", key: {_id: "hashed"}}]});
assert.commandFailedWithCode(res, ErrorCodes.CommandNotSupportedOnView);

// Test that user is not allowed to create indexes in config.transactions.
const configDB = db.getSiblingDB("config");
res = configDB.runCommand({createIndexes: "transactions", indexes: [{key: {star: 1}, name: "star"}]});
assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);

// Test that providing an empty list of index spec for config.transactions should also fail with
// IllegalOperation, rather than BadValue for a normal collection.
// This is consistent with server behavior prior to 6.0.
res = configDB.runCommand({createIndexes: "transactions", indexes: []});
assert.commandFailedWithCode(res, ErrorCodes.IllegalOperation);
