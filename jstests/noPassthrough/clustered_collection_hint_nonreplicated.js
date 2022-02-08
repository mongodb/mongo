/**
 * Tests that a collection with a clustered index can use and interpret a query hint.
 * @tags: [
 *   requires_fcv_52,
 *   # Does not support sharding
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 * ]
 */
(function() {
"use strict";
load("jstests/libs/clustered_collections/clustered_collection_util.js");
load("jstests/libs/clustered_collections/clustered_collection_hint_common.js");

const conn = MongoRunner.runMongod({setParameter: {supportArbitraryClusterKeyIndex: true}});

const nonReplicatedDB = conn.getDB("local");
const collName = "coll";
const nonReplicatedColl = nonReplicatedDB[collName];

testClusteredCollectionHint(nonReplicatedColl, {ts: 1}, "ts_1");

MongoRunner.stopMongod(conn);
})();
