/**
 * Tests createIndexes with the 'clustered' option on a replicated collection.
 *
 * @tags: [
 *   requires_fcv_53,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   # Does not support sharding
 *   assumes_against_mongod_not_mongos,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/libs/clustered_collections/clustered_collection_create_index_clustered_common.js");

const replicatedDB = db.getSiblingDB("create_index_clustered");
const collName = "coll";

CreateIndexesClusteredTest.runBaseTests(replicatedDB, collName);
})();
