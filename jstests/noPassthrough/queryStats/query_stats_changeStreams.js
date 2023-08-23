// Tests the collection of query stats for a change stream query.
// @tags: [
//   uses_change_streams,
//   requires_replication,
//   featureFlagQueryStats
// ]
import {ChangeStreamTest} from "jstests/libs/change_stream_util.js";
import {getQueryStatsAggCmd} from "jstests/libs/query_stats_utils.js";

// TODO SERVER-76263 also run this on a sharded cluster
function runTest(conn) {
    const db = conn.getDB("test");
    const coll = db.coll;
    coll.drop();

    // Create a changeStream collection
    let cst = new ChangeStreamTest(db);
    cst.startWatchingChanges({
        pipeline: [{$changeStream: {}}],
        collection: coll,
    });
    cst.cleanUp();

    const queryStats = getQueryStatsAggCmd(db);
    assert.eq(1, queryStats.length);
    assert.eq("changeStream", queryStats[0].key.collectionType);
    assert.eq("oplog.rs", queryStats[0].key.queryShape.cmdNs.coll);
}

const rst = new ReplSetTest({nodes: 2});
rst.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
rst.initiate();
runTest(rst.getPrimary());
rst.stopSet();
