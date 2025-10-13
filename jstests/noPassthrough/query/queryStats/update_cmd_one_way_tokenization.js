/**
 * Test that $queryStats properly tokenizes update commands on mongod.
 *
 * @tags: [requires_fcv_83]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getQueryStatsUpdateCmd, withQueryStatsEnabled} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4=";

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();

    if (testDB.getMongo().isMongos()) {
        // TODO SERVER-112050 Unskip this when we support sharded clusters for update.
        jsTest.log.info("Skipping update tokenization test on sharded cluster");
        return;
    }

    coll.drop();
    assert.commandWorked(coll.insert({v: 1}));

    const cmdObj = {
        update: collName,
        updates: [{q: {v: 1}, u: {v: 100}}],
        comment: "test comment!",
    };
    assert.commandWorked(testDB.runCommand(cmdObj));

    let queryStats = getQueryStatsUpdateCmd(testDB, {transformIdentifiers: true});

    if (!FeatureFlagUtil.isPresentAndEnabled(testDB, "QueryStatsUpdateCommand")) {
        assert.eq(queryStats, [], "expect no query stats for update when feature flag is off");
        return;
    }

    assert.eq(1, queryStats.length);
    assert.eq("update", queryStats[0].key.queryShape.command);
    assert.eq({[kHashedFieldName]: {$eq: "?number"}}, queryStats[0].key.queryShape.q);
    assert.eq("?object", queryStats[0].key.queryShape.u);
});
