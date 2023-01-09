/**
 * Tests inserting various cluster key values, duplicates, updates and secondary index lookups
 * on a collection clustered by {_id: 1}.
 *
 * @tags: [
 *   assumes_against_mongod_not_mongos,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   requires_fcv_53,
 *   tenant_migration_incompatible, #TODO: why is it incompatible?
 * ]
 */

(function() {
"use strict";

load("jstests/libs/clustered_collections/clustered_collection_util.js");

const replicatedDB = db.getSiblingDB('replicated');
const collName = 'clustered_collection';
const replicatedColl = replicatedDB[collName];

replicatedColl.drop();

ClusteredCollectionUtil.testBasicClusteredCollection(replicatedDB, collName, '_id');
})();
