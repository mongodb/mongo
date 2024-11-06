/**
 * Tests support for the configureQueryAnalyzer command.
 *
 * @tags: [
 *   requires_fcv_70,
 *   assumes_read_concern_unchanged
 * ]
 */

import {
    testExistingCollection,
    testNonExistingCollection
} from "jstests/sharding/analyze_shard_key/libs/configure_query_analyzer_common.js";

const dbName = jsTestName();

const testCases = [];
testCases.push({conn: db.getMongo(), isCoreTest: true});

testNonExistingCollection(testCases, dbName);
testExistingCollection(db.getMongo(), testCases, dbName);
