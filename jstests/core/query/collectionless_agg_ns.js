/**
 * Tests commands run against the $cmd.aggregate namespace.
 * @tags: [
 *   # Some of these commands produce "cannot run within a multi-document transaction"
 *   does_not_support_transactions,
 *   requires_fcv_83,
 *   # Tests that implicitly create/drop collections will fail on the $cmd.aggregate namespace
 *   assumes_no_implicit_collection_creation_on_get_collection,
 *   # Refusing to run commands because it relies on in-memory state
 *   does_not_support_stepdowns,
 *   assumes_no_implicit_cursor_exhaustion,
 *   # There is no need to support multitenancy, as it has been canceled and was never in
 *   # production (see SERVER-97215 for more information)
 *   command_not_supported_in_serverless,
 *   # Views cannot be created on the $cmd.aggregate namespace
 *   incompatible_with_views,
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {describe, it} from "jstests/libs/mochalite.js";

const collName = "collectionless_agg_ns_test";
db[collName].insert({a: 1}); // to avoid empty db
const cursor = assert.commandWorked(
    db.runCommand({aggregate: 1, pipeline: [{$documents: [{a: 1}, {a: 2}]}], cursor: {batchSize: 1}}),
);

describe("Commands that are permitted on the $cmd.aggregate namespace", function () {
    const commands = [
        {aggregate: 1, pipeline: [{$documents: [{a: 1}]}], cursor: {}},
        {
            aggregate: 1,
            pipeline: [
                {$documents: [{a: 1}]},
                {$lookup: {from: "$cmd.aggregate", pipeline: [{$documents: [{a: 1}]}], as: "res"}},
            ],
            cursor: {},
        },
        {aggregate: 1, pipeline: [{$listClusterCatalog: {}}], cursor: {}},
        {aggregate: 1, pipeline: [{$listLocalSessions: {}}], cursor: {}},
        {getMore: cursor.cursor.id, collection: "$cmd.aggregate"},
        {killCursors: "$cmd.aggregate", cursors: [NumberLong(12345)]},
    ];
    // checkMetadataConsistency uses a special namespace under the hood.
    if (FixtureHelpers.isMongos(db)) {
        commands.push({checkMetadataConsistency: 1});
    }

    commands.forEach((command) => {
        it(`should succeed with command ${tojson(command)}`, function () {
            assert.commandWorked(db.runCommand(command));
        });
    });

    const adminCommands = [{aggregate: 1, pipeline: [{$currentOp: {}}], cursor: {}}];
    adminCommands.forEach((command) => {
        it(`should succeed with command ${tojson(command)}`, function () {
            assert.commandWorked(db.adminCommand(command));
        });
    });
});

// TODO SERVER-110021: Ensure these commands reject the $cmd.aggregate namespace (ensure command
// failed with invalid namespace code, or similar).
describe("Commands not permitted on the $cmd.aggregate namespace", function () {
    const commands = [
        {find: "$cmd.aggregate", filter: {a: 1}},
        {explain: {find: "$cmd.aggregate", filter: {a: 1}}},
        {aggregate: "$cmd.aggregate", pipeline: [{$documents: [{a: 1}]}], cursor: {}},
        {aggregate: collName, pipeline: [{$out: "$cmd.aggregate"}], cursor: {}},
        {distinct: "$cmd.aggregate", key: "a"},
        {count: "$cmd.aggregate", query: {a: 1}},
        {update: "$cmd.aggregate", updates: [{q: {a: 1}, u: {$set: {b: 1}}}]},
        {delete: "$cmd.aggregate", deletes: [{q: {a: 1}, limit: 0}]},
        {findAndModify: "$cmd.aggregate", query: {a: 1}, update: {$set: {b: 1}}},
        {
            mapReduce: "$cmd.aggregate",
            map: "function() {}",
            reduce: "function() {}",
            out: {inline: 1},
        },
        {mapReduce: collName, map: "function() {}", reduce: "function() {}", out: "$cmd.aggregate"},

        {planCacheClear: "$cmd.aggregate"},
        {planCacheListFilters: "$cmd.aggregate"},
        {planCacheClearFilters: "$cmd.aggregate", query: {a: 1}, sort: {}, projection: {}},
        {
            planCacheSetFilter: "$cmd.aggregate",
            query: {a: 1},
            sort: {},
            projection: {},
            indexHints: [{a: 1}],
        },

        {dataSize: "test.$cmd.aggregate"},
        {collStats: "$cmd.aggregate"},
        {validate: "$cmd.aggregate"},

        {create: "$cmd.aggregate"},
        {drop: "$cmd.aggregate"},
        {insert: "$cmd.aggregate", documents: [{a: 1}]},
        {createIndexes: "$cmd.aggregate", indexes: [{key: {a: 1}, name: "a_1"}]},
        {dropIndexes: "$cmd.aggregate", index: "a_1"},
        {listIndexes: "$cmd.aggregate"},
        {collMod: "$cmd.aggregate", validator: {a: {$gt: 5}}},
        {create: "view", viewOn: "$cmd.aggregate", pipeline: [{$match: {a: 1}}]},
        {enableSharding: "$cmd.aggregate"},
    ];

    // When auth is enabled, running some commands with an invalid namespace will produce a special
    // error during the auth check, rather than the generic 'InvalidNamespace' error.
    commands.forEach((command) => {
        it(`should not tassert fail with command ${tojson(command)}`, function () {
            db.runCommand(command);
        });
    });

    const adminCommands = [
        {renameCollection: "test.$cmd.aggregate", to: "test.foo"},
        {renameCollection: "test.foo", to: "test.$cmd.aggregate"},
        {bulkWrite: 1, ops: [{insert: 0, document: {a: 1}}], nsInfo: [{ns: "test.$cmd.aggregate"}]},
    ];
    if (FixtureHelpers.isMongos(db)) {
        adminCommands.push(
            {shardCollection: "test.$cmd.aggregate", key: {a: 1}},
            {analyzeShardKey: "test.$cmd.aggregate", key: {a: 1}},
        );
    }
    adminCommands.forEach((command) => {
        it(`should not fail with command ${tojson(command)}`, function () {
            db.runCommand(command);
        });
    });
});
