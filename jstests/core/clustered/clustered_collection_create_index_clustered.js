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
import {
    CreateIndexesClusteredTest
} from "jstests/libs/clustered_collections/clustered_collection_create_index_clustered_common.js";

const replicatedDB = db.getSiblingDB(jsTestName());
const collName = "coll";

CreateIndexesClusteredTest.runBaseTests(replicatedDB, collName);