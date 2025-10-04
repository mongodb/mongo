// Cannot implicitly shard accessed collections because the error response from the shard about
// using the empty string as the out collection name is converted to an error and no longer retains
// the "code" property.
// @tags: [
//   # This test runs commands that are not allowed with security token: cleanupOrphaned,
//   # cloneCollectionAsCapped, compact, dataSize, enableSharding, godinsert, mapreduce,
//   # mergeChunks, moveChunk, movePrimary, planCacheClear, planCacheClearFilters,
//   # planCacheListFilters, planCacheSetFilter, reIndex, shardCollection, split,
//   # updateZoneKeyRange.
//   not_allowed_with_signed_security_token,
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
//   requires_fastcount,
//   requires_getmore,
//   requires_non_retryable_commands,
//   uses_testing_only_commands,
//   uses_map_reduce_with_temp_collections,
//   no_selinux,
//   # This test has statements that do not support non-local read concern.
//   does_not_support_causal_consistency,
//   uses_compact,
// ]
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

// This file tests that commands namespace parsing rejects embedded null bytes.
// Note that for each command, a properly formatted command object must be passed to the helper
// function, regardless of the namespace used in the command object.

const isFullyQualified = true;
const isNotFullyQualified = false;
const isAdminCommand = true;
const isNotAdminCommand = false;

// If the command expects the namespace to be fully qualified, set `isFullyQualified` to true.
// If the command must be run against the admin database, set `isAdminCommand` to true.
function assertFailsWithInvalidNamespacesForField(field, command, isFullyQualified, isAdminCommand) {
    const invalidNamespaces = [];
    invalidNamespaces.push(isFullyQualified ? "mydb." : "");
    invalidNamespaces.push(isFullyQualified ? "mydb.\0" : "\0");
    invalidNamespaces.push(isFullyQualified ? "mydb.a\0b" : "a\0b");

    const cmds = [];
    for (let ns of invalidNamespaces) {
        const cmd = Object.extend({}, command, /* deep copy */ true);

        const fieldNames = field.split(".");
        const lastFieldNameIndex = fieldNames.length - 1;
        let objToUpdate = cmd;
        for (let i = 0; i < lastFieldNameIndex; i++) {
            objToUpdate = objToUpdate[fieldNames[i]];
        }
        objToUpdate[fieldNames[lastFieldNameIndex]] = ns;

        cmds.push(cmd);
    }

    const dbCmd = isAdminCommand ? db.adminCommand : db.runCommand;
    const expErrors = [ErrorCodes.InvalidNamespace, ErrorCodes.NamespaceNotFound];
    for (let cmd of cmds) {
        jsTestLog(`running ${tojson(cmd)}`);
        assert.commandFailedWithCode(dbCmd.apply(db, [cmd]), expErrors);
    }
}

const runningOnMongos = FixtureHelpers.isMongos(db);

db.commands_namespace_parsing.drop();
assert.commandWorked(db.commands_namespace_parsing.insert({a: 1}));

// Test aggregate fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "aggregate",
    {aggregate: "", pipeline: []},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test aggregate with stages which involve foreign collections fail with an invalid collection
// name.
assertFailsWithInvalidNamespacesForField(
    "pipeline.$out",
    {aggregate: "commands_namespace_parsing", pipeline: [{$out: ""}], cursor: {}},
    isNotFullyQualified,
    isNotAdminCommand,
);
assertFailsWithInvalidNamespacesForField(
    "pipeline.$merge",
    {aggregate: "commands_namespace_parsing", pipeline: [{$merge: ""}], cursor: {}},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test count fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("count", {count: ""}, isNotFullyQualified, isNotAdminCommand);

// Test distinct fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("distinct", {distinct: "", key: "a"}, isNotFullyQualified, isNotAdminCommand);

// Test mapReduce fails with an invalid input collection name.
assertFailsWithInvalidNamespacesForField(
    "mapreduce",
    {
        mapreduce: "",
        map: function () {
            emit(this.a, 1);
        },
        reduce: function (key, values) {
            return Array.sum(values);
        },
        out: "commands_namespace_parsing_out",
    },
    isNotFullyQualified,
    isNotAdminCommand,
);
// Test mapReduce fails with an invalid output collection name.
assertFailsWithInvalidNamespacesForField(
    "out",
    {
        mapreduce: "commands_namespace_parsing",
        map: function () {
            emit(this.a, 1);
        },
        reduce: function (key, values) {
            return Array.sum(values);
        },
        out: "",
    },
    isNotFullyQualified,
    isNotAdminCommand,
);
assertFailsWithInvalidNamespacesForField(
    "out.merge",
    {
        mapreduce: "commands_namespace_parsing",
        map: function () {
            emit(this.a, 1);
        },
        reduce: function (key, values) {
            return Array.sum(values);
        },
        out: {merge: ""},
    },
    isNotFullyQualified,
    isNotAdminCommand,
);
assertFailsWithInvalidNamespacesForField(
    "out.reduce",
    {
        mapreduce: "commands_namespace_parsing",
        map: function () {
            emit(this.a, 1);
        },
        reduce: function (key, values) {
            return Array.sum(values);
        },
        out: {reduce: ""},
    },
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test find fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("find", {find: ""}, isNotFullyQualified, isNotAdminCommand);

// Test insert fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "insert",
    {insert: "", documents: [{q: {a: 1}, u: {a: 2}}]},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test update fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "update",
    {update: "", updates: [{q: {a: 1}, u: {a: 2}}]},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test delete fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "delete",
    {delete: "", deletes: [{q: {a: 1}, limit: 1}]},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test findAndModify fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "findAndModify",
    {findAndModify: "", update: {a: 2}},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test getMore fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "collection",
    {getMore: NumberLong("123456"), collection: ""},
    isNotFullyQualified,
    isNotAdminCommand,
);

if (!runningOnMongos) {
    // Test godinsert fails with an invalid collection name.
    assertFailsWithInvalidNamespacesForField(
        "godinsert",
        {godinsert: "", obj: {_id: 1}},
        isNotFullyQualified,
        isNotAdminCommand,
    );
}

// Test planCacheListFilters fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "planCacheListFilters",
    {planCacheListFilters: ""},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test planCacheSetFilter fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "planCacheSetFilter",
    {planCacheSetFilter: "", query: {}, indexes: [{a: 1}]},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test planCacheClearFilters fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "planCacheClearFilters",
    {planCacheClearFilters: ""},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test planCacheClear fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "planCacheClear",
    {planCacheClear: ""},
    isNotFullyQualified,
    isNotAdminCommand,
);

if (!runningOnMongos) {
    // Test cleanupOrphaned fails with an invalid collection name.
    assertFailsWithInvalidNamespacesForField(
        "cleanupOrphaned",
        {cleanupOrphaned: ""},
        isFullyQualified,
        isAdminCommand,
    );
}

if (runningOnMongos) {
    // Test enableSharding fails with an invalid database name.
    assertFailsWithInvalidNamespacesForField(
        "enableSharding",
        {enableSharding: ""},
        isNotFullyQualified,
        isAdminCommand,
    );

    // Test mergeChunks fails with an invalid collection name.
    assertFailsWithInvalidNamespacesForField(
        "mergeChunks",
        {mergeChunks: "", bounds: [{_id: MinKey()}, {_id: MaxKey()}]},
        isFullyQualified,
        isAdminCommand,
    );

    // Test shardCollection fails with an invalid collection name.
    assertFailsWithInvalidNamespacesForField(
        "shardCollection",
        {shardCollection: "", key: {_id: 1}},
        isFullyQualified,
        isAdminCommand,
    );

    // Test split fails with an invalid collection name.
    assertFailsWithInvalidNamespacesForField("split", {split: "", find: {}}, isFullyQualified, isAdminCommand);

    // Test moveChunk fails with an invalid collection name.
    const shardName = db.adminCommand({listShards: 1}).shards[0]._id;
    assertFailsWithInvalidNamespacesForField(
        "moveChunk",
        {moveChunk: "", find: {}, to: shardName},
        isNotFullyQualified,
        isAdminCommand,
    );

    // Test movePrimary fails with an invalid database name.
    assertFailsWithInvalidNamespacesForField(
        "movePrimary",
        {movePrimary: "", to: "dummy"},
        isNotFullyQualified,
        isAdminCommand,
    );

    // Test updateZoneKeyRange fails with an invalid collection name.
    assertFailsWithInvalidNamespacesForField(
        "updateZoneKeyRange",
        {updateZoneKeyRange: "", min: {_id: MinKey()}, max: {_id: MaxKey()}, zone: "3"},
        isNotFullyQualified,
        isAdminCommand,
    );
}

// Test renameCollection fails with an invalid source collection name.
assertFailsWithInvalidNamespacesForField(
    "renameCollection",
    {renameCollection: "", to: "test.b"},
    isFullyQualified,
    isAdminCommand,
);
// Test renameCollection fails with an invalid target collection name.
assertFailsWithInvalidNamespacesForField("to", {renameCollection: "test.b", to: ""}, isFullyQualified, isAdminCommand);

// Test drop fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("drop", {drop: ""}, isNotFullyQualified, isNotAdminCommand);

// Test create fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("create", {create: ""}, isNotFullyQualified, isNotAdminCommand);

// TODO SERVER-85773: Enale below test for sharded clusters.
if (!runningOnMongos) {
    // Test cloneCollectionAsCapped fails with an invalid source collection name.
    assertFailsWithInvalidNamespacesForField(
        "cloneCollectionAsCapped",
        {cloneCollectionAsCapped: "", toCollection: "b", size: 1024},
        isNotFullyQualified,
        isNotAdminCommand,
    );
    // Test cloneCollectionAsCapped fails with an invalid target collection name.
    assertFailsWithInvalidNamespacesForField(
        "toCollection",
        {cloneCollectionAsCapped: "commands_namespace_parsing", toCollection: "", size: 1024},
        isNotFullyQualified,
        isNotAdminCommand,
    );
}

// Test convertToCapped fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "convertToCapped",
    {convertToCapped: "", size: 1024},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test filemd5 fails with an invalid collection name.
// Note: for this command, it is OK to pass 'root: ""', so do not use the helper function.
assert.commandFailedWithCode(db.runCommand({filemd5: ObjectId(), root: "\0"}), ErrorCodes.InvalidNamespace);
assert.commandFailedWithCode(db.runCommand({filemd5: ObjectId(), root: "a\0b"}), ErrorCodes.InvalidNamespace);

// Test createIndexes fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "createIndexes",
    {createIndexes: "", indexes: [{key: {a: 1}, name: "a1"}]},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test listIndexes fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("listIndexes", {listIndexes: ""}, isNotFullyQualified, isNotAdminCommand);

// Test dropIndexes fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "dropIndexes",
    {dropIndexes: "", index: "*"},
    isNotFullyQualified,
    isNotAdminCommand,
);

if (!runningOnMongos) {
    // Test compact fails with an invalid collection name.
    assertFailsWithInvalidNamespacesForField("compact", {compact: ""}, isNotFullyQualified, isNotAdminCommand);
}

// Test collMod fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "collMod",
    {collMod: "", index: {keyPattern: {a: 1}, expireAfterSeconds: 60}},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test reIndex fails with an invalid collection name.
if (!runningOnMongos) {
    assertFailsWithInvalidNamespacesForField("reIndex", {reIndex: ""}, isNotFullyQualified, isNotAdminCommand);
}

// Test collStats fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("collStats", {collStats: ""}, isNotFullyQualified, isNotAdminCommand);

// Test dataSize fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("dataSize", {dataSize: ""}, isFullyQualified, isNotAdminCommand);

// Test explain of aggregate fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "aggregate",
    {aggregate: "", pipeline: [], explain: true},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test explain of count fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "explain.count",
    {explain: {count: ""}},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test explain of distinct fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "explain.distinct",
    {explain: {distinct: "", key: "a"}},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test explain of find fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("explain.find", {explain: {find: ""}}, isNotFullyQualified, isNotAdminCommand);

// Test explain of findAndModify fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "explain.findAndModify",
    {explain: {findAndModify: "", update: {a: 2}}},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test explain of delete fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "explain.delete",
    {explain: {delete: "", deletes: [{q: {a: 1}, limit: 1}]}},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test explain of update fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField(
    "explain.update",
    {explain: {update: "", updates: [{q: {a: 1}, u: {a: 2}}]}},
    isNotFullyQualified,
    isNotAdminCommand,
);

// Test validate fails with an invalid collection name.
assertFailsWithInvalidNamespacesForField("validate", {validate: ""}, isNotFullyQualified, isNotAdminCommand);
