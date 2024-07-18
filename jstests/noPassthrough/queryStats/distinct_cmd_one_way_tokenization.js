/**
 * Test that $queryStats properly tokenizes distinct commands on mongod and mongos.
 *
 * @tags: [featureFlagQueryStatsCountDistinct]
 */
import {getQueryStatsDistinctCmd, withQueryStatsEnabled} from "jstests/libs/query_stats_utils.js";

const collName = jsTestName();

const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4=";

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();
    coll.drop();
    coll.insert({v: 1});

    coll.distinct("v");
    let queryStats = getQueryStatsDistinctCmd(testDB, {transformIdentifiers: true});
    assert.eq(1, queryStats.length);
    assert.eq("distinct", queryStats[0].key.queryShape.command);
    assert.eq(kHashedFieldName, queryStats[0].key.queryShape.key);

    coll.insert({v: 2});
    coll.insert({v: 5});

    coll.distinct("v", {"$or": [{"v": {$gt: 3}}, {"v": {$eq: 2}}]});
    queryStats = getQueryStatsDistinctCmd(testDB, {transformIdentifiers: true});

    assert.eq(2, queryStats.length);
    assert.eq("distinct", queryStats[1].key.queryShape.command);
    assert.eq(kHashedFieldName, queryStats[1].key.queryShape.key);
    assert.eq({
        "$or": [{[kHashedFieldName]: {"$gt": "?number"}}, {[kHashedFieldName]: {"$eq": "?number"}}]
    },
              queryStats[1].key.queryShape.query);
});
