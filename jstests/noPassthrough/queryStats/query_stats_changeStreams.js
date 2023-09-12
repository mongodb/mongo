// Tests the collection of query stats for a change stream query.
// @tags: [
//   uses_change_streams,
//   requires_replication,
//   requires_sharding,
//   featureFlagQueryStats
// ]
import {ChangeStreamTest} from "jstests/libs/change_stream_util.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getQueryStatsAggCmd} from "jstests/libs/query_stats_utils.js";

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
    assert.eq(coll.getName(), queryStats[0].key.queryShape.cmdNs.coll);

    // TODO SERVER-76263 Support reporting 'collectionType' on a sharded cluster.
    if (!FixtureHelpers.isMongos(db)) {
        assert.eq("changeStream", queryStats[0].key.collectionType);
    }
}

{
    // Test the non-sharded case.
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet({setParameter: {internalQueryStatsRateLimit: -1}});
    rst.initiate();
    runTest(rst.getPrimary());
    rst.stopSet();
}

{
    // Test on a sharded cluster.
    const st = new ShardingTest({
        mongos: 1,
        shards: 1,
        config: 1,
        rs: {nodes: 1},
        mongosOptions: {
            setParameter: {
                internalQueryStatsRateLimit: -1,
            }
        },
    });
    runTest(st.s);
    st.stop();
}