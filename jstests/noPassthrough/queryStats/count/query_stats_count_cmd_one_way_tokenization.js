/**
 * Test that $queryStats properly tokenizes (i.e. anonymizes) count commands on mongod.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */

import {getQueryStatsCountCmd} from "jstests/libs/query_stats_utils.js";

const options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};
const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB("test");

assert.commandWorked(testDB.runCommand({count: jsTestName(), query: {a: 1}}));

const stats = getQueryStatsCountCmd(conn, {transformIdentifiers: true});
assert.eq(1, stats.length, stats);

// Check that sensitive data elements are substituted with HMAC value
const kHashedFieldName = "GDiF6ZEXkeo4kbKyKEAAViZ+2RHIVxBQV9S6b6Lu7gU=";
assert.eq({[kHashedFieldName]: {"$eq": "?number"}}, stats[0].key.queryShape.query, stats);

MongoRunner.stopMongod(conn);

// TODO SERVER-90655: Add test for queryStats tokenization on mongos
