/**
 * Test that $queryStats properly tokenizes update (where the update modification is specified
 * as replacement document or pipeline) commands on mongod.
 *
 * @tags: [requires_fcv_83]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {
    getQueryStatsUpdateCmd,
    withQueryStatsEnabled,
    resetQueryStatsStore,
} from "jstests/libs/query/query_stats_utils.js";

const collName = jsTestName();

const kHashedFieldName = "lU7Z0mLRPRUL+RfAD5jhYPRRpXBsZBxS/20EzDwfOG4="; // Hash of field "v"
const kHashedIdField = "+0wgDp/AI7f+XT+DJEqixDyZBq9zRe7RGN0wCS9bd94="; // Hash of _id

//
// Replacement update tests.
//

function testReplacementUpdateTokenization(testDB, collName) {
    const cmdObj = {
        update: collName,
        updates: [{q: {v: 1}, u: {v: 100}}],
        comment: "replacement update!",
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
}

//
// Pipeline update tests.
//

/**
 * Test with all possible allowed aggregation stages in a pipeline-style update:
 * - $addFields
 * - $project
 * - $replaceRoot
 */
function testPipelineUpdateTokenization(testDB, collName) {
    const cmdObj = {
        update: collName,
        updates: [
            {
                q: {v: 1},
                u: [{$addFields: {v: 2}}, {$project: {_id: 1}}, {$replaceRoot: {newRoot: {v: 3}}}],
            },
        ],
        comment: "pipeline update!",
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

    const expectedU = [
        {$addFields: {[kHashedFieldName]: "?number"}},
        {$project: {"+0wgDp/AI7f+XT+DJEqixDyZBq9zRe7RGN0wCS9bd94=": true}},
        {$replaceRoot: {newRoot: "?object"}},
    ];
    assert.eq(expectedU, queryStats[0].key.queryShape.u);
}

/**
 * Test with the aliases of all possible allowed aggregation stages in a pipeline-style update.
 * - $set is an alias for $addFields
 * - $unset is an alias for $project
 * - $replaceWith is an alias for $replaceRoot
 */
function testPipelineUpdateWithAliasesTokenization(testDB, collName) {
    const cmdObj = {
        update: collName,
        updates: [
            {
                q: {v: 1},
                u: [{$set: {v: "newValue"}}, {$unset: "v"}, {$replaceWith: {newDoc: {v: 5}}}],
            },
        ],
        comment: "pipeline with stage aliases update!",
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

    const expectedU = [
        {$set: {[kHashedFieldName]: "?string"}},
        {$project: {[kHashedFieldName]: false, [kHashedIdField]: true}}, // $unset aliases to exclusion $project.
        {$replaceRoot: {newRoot: "?object"}}, // $replaceWith aliases to $replaceRoot.
    ];
    assert.eq(expectedU, queryStats[0].key.queryShape.u);
}

/**
 * Test that a constant in a pipeline-style update is tokenized.
 */
function testPipelineUpdateWithConstantTokenization(testDB, collName) {
    const kHashedConstant = "$$VaksLvxpslthbM1qCItU0mhsNVyN8A97qAJzqIKrZoI="; // Hash of "$$newVal"
    const cmdObj = {
        update: collName,
        updates: [
            {
                q: {v: 1},
                u: [{$set: {v: "$$newVal"}}],
                c: {newVal: 500},
            },
        ],
        comment: "pipeline update with constants!",
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
    assert.eq([{$set: {[kHashedFieldName]: kHashedConstant}}], queryStats[0].key.queryShape.u);
    assert.eq({newVal: "?number"}, queryStats[0].key.queryShape.c);
}

withQueryStatsEnabled(collName, (coll) => {
    const testDB = coll.getDB();

    if (testDB.getMongo().isMongos()) {
        // TODO SERVER-112050 Unskip this when we support sharded clusters for update.
        jsTest.log.info("Skipping update tokenization test on sharded cluster");
        return;
    }

    coll.drop();
    assert.commandWorked(coll.insert({v: 1}));

    jsTest.log.info("Testing replacement update");
    testReplacementUpdateTokenization(testDB, collName);

    jsTest.log.info("Testing pipeline update");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testPipelineUpdateTokenization(testDB, collName);

    jsTest.log.info("Testing pipeline update with aliases");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testPipelineUpdateWithAliasesTokenization(testDB, collName);

    jsTest.log.info("Testing pipeline update with constant");
    resetQueryStatsStore(testDB.getMongo(), "1MB");
    testPipelineUpdateWithConstantTokenization(testDB, collName);
});
