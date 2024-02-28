/**
 * Verifies bounded collection scan operation for collections clustered by _id.
 *
 * @tags: [
 *   # The expected doc examined is incorrect in multiversion tests with mongod below 8.0.
 *   requires_fcv_80,
 *   # Does not support sharding
 *   assumes_against_mongod_not_mongos,
 *   assumes_unsharded_collection,
 * ]
 */
import {
    testClusteredCollectionBoundedScan
} from "jstests/libs/clustered_collections/clustered_collection_bounded_scan_common.js";

const replicatedDB = db.getSiblingDB(jsTestName());
const collName = "coll";
const replicatedColl = replicatedDB[collName];

testClusteredCollectionBoundedScan(replicatedColl, {_id: 1});