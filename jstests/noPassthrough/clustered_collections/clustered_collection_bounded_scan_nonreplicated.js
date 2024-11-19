/**
 * Verifies bounded collection scan operation for collections clustered by arbitrary keys.
 *
 * @tags: [
 *   requires_fcv_53,
 *   # Does not support sharding
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   requires_profiling,
 * ]
 */
import {
    testClusteredCollectionBoundedScan
} from "jstests/libs/clustered_collections/clustered_collection_bounded_scan_common.js";

const conn = MongoRunner.runMongod({setParameter: {supportArbitraryClusterKeyIndex: true}});

const nonReplicatedDB = conn.getDB("local");
const collName = "coll";
const nonReplicatedColl = nonReplicatedDB[collName];

testClusteredCollectionBoundedScan(nonReplicatedColl, {ts: 1}, true /*checkProfile*/);

MongoRunner.stopMongod(conn);
