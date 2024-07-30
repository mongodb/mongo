/**
 * Test that $queryStats properly tokenizes (i.e. anonymizes) count commands on mongod.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */

import {getQueryStatsCountCmd, withQueryStatsEnabled} from "jstests/libs/query_stats_utils.js";

withQueryStatsEnabled(jsTestName(), (coll) => {
    const testDB = coll.getDB();
    assert.commandWorked(testDB.runCommand({count: jsTestName(), query: {a: 1}}));

    const stats = getQueryStatsCountCmd(testDB, {transformIdentifiers: true});
    assert.eq(1, stats.length, stats);

    // Check that sensitive data elements are substituted with HMAC value
    const kHashedFieldName = "GDiF6ZEXkeo4kbKyKEAAViZ+2RHIVxBQV9S6b6Lu7gU=";
    assert.eq({[kHashedFieldName]: {"$eq": "?number"}}, stats[0].key.queryShape.query, stats);
});
