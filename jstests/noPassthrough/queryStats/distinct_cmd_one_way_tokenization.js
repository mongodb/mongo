/**
 * Test that $queryStats properly tokenizes distinct commands on mongod.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */
import {getQueryStatsDistinctCmd} from "jstests/libs/query_stats_utils.js";

const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4=";

function runTest(conn) {
    const db = conn.getDB("test");
    const admin = conn.getDB("admin");

    db.test.drop();
    db.test.insert({v: 1});

    db.test.distinct("v");
    let queryStats = getQueryStatsDistinctCmd(admin, {transformIdentifiers: true});

    assert.eq(1, queryStats.length);
    assert.eq("distinct", queryStats[0].key.queryShape.command);
    assert.eq(kHashedFieldName, queryStats[0].key.queryShape.key);

    db.test.insert({v: 2});
    db.test.insert({v: 5});

    db.test.distinct("v", {"$or": [{"v": {$gt: 3}}, {"v": {$eq: 2}}]});
    queryStats = getQueryStatsDistinctCmd(admin, {transformIdentifiers: true});

    assert.eq(2, queryStats.length);
    assert.eq("distinct", queryStats[1].key.queryShape.command);
    assert.eq(kHashedFieldName, queryStats[1].key.queryShape.key);
    assert.eq({
        "$or": [{[kHashedFieldName]: {"$gt": "?number"}}, {[kHashedFieldName]: {"$eq": "?number"}}]
    },
              queryStats[1].key.queryShape.query);
}

let conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsRateLimit: -1,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

// TODO SERVER-90650: Add sharded cluster test once distinct queryStats on mongos is supported
