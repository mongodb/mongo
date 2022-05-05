/**
 * Tests inserting various cluster key values, duplicates, updates and secondary index lookups
 * on a non-replicated collection clustered by {ts: 1}.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   does_not_support_causal_consistency,
 *   does_not_support_stepdowns,
 *   requires_fcv_53,
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   tenant_migration_incompatible, #TODO: why is it incompatible?
 * ]
 */

(function() {
"use strict";

load("jstests/libs/clustered_collections/clustered_collection_util.js");

const conn = MongoRunner.runMongod({setParameter: {supportArbitraryClusterKeyIndex: true}});

const nonReplicatedDB = conn.getDB('local');
const collName = 'clustered_collection';
const nonReplicatedColl = nonReplicatedDB[collName];

nonReplicatedColl.drop();

ClusteredCollectionUtil.testBasicClusteredCollection(nonReplicatedDB, collName, 'ts');

MongoRunner.stopMongod(conn);
})();
