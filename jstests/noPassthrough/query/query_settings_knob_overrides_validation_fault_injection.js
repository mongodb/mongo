/**
 * Fault-injection tests for query knob validation using the failQueryKnobOverridesParsing
 * failpoint:
 *  1. [good primary / bad secondary] replica set: the secondary strips the bad knob and logs an
 *     error, while the valid sibling knob still applies.
 *  2. [good mongos / bad configsvr] sharded cluster: the configsvr rejects the setQuerySettings
 *     command and the error propagates back to the client via mongos.
 *  3. [good mongos / bad shard] sharded cluster: the shard strips the bad knob and logs an error,
 *     while the query keeps executing with the valid sibling knob applied.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 *   directly_against_shardsvrs_incompatible,
 *   featureFlagPqsQueryKnobs,
 *   requires_fcv_90,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kFailPointName = "failQueryKnobOverridesParsing";
// Logged by QuerySettingsClusterParameter::set() when applying an already-accepted cluster
// parameter value (oplog application / startup load) whose queryKnobs failed to parse.
const kClusterParameterApplyLogId = 12978700;

// samplingMarginOfError (double, valid range [1, 10]) is the knob targeted by the failpoint.
// cbrCEMode (enum) is the sibling knob that must survive the fault injection.
const kBadKnobName = "samplingMarginOfError";
const kBadKnobValue = 3.0;
const kGoodKnobName = "cbrCEMode";
const kGoodKnobValue = "heuristicCE";

function makeSettings() {
    return {queryKnobs: {[kBadKnobName]: kBadKnobValue, [kGoodKnobName]: kGoodKnobValue}};
}

function assertGoodKnobOnlyInExplain(qsutils, query) {
    qsutils.assertExplainQuerySettings(query, {
        queryKnobs: {[kGoodKnobName]: kGoodKnobValue},
    });
}

function runGoodPrimaryBadSecondaryTest() {
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();
    const primaryDb = primary.getDB(jsTestName());
    const coll = assertDropAndRecreateCollection(primaryDb, jsTestName());
    const qsutils = new QuerySettingsUtils(primaryDb, coll.getName());
    qsutils.removeAllQuerySettings();

    const query = qsutils.makeFindQueryInstance({filter: {a: 1}});

    // Arm the failpoint on the secondary only, so the primary accepts and persists the setting,
    // but the secondary fails to re-parse the offending knob when it applies the replicated
    // cluster-parameter update.
    const fp = configureFailPoint(secondary, kFailPointName, {name: kBadKnobName});

    assert.commandWorked(
        primaryDb.adminCommand({setQuerySettings: query, settings: makeSettings()}),
    );
    rst.awaitReplication();

    assert(
        checkLog.checkContainsOnceJson(secondary, kClusterParameterApplyLogId, {
            reasons: (reasons) => reasons.some((r) => r.includes(kBadKnobName)),
        }),
    );

    const secondaryQsutils = new QuerySettingsUtils(secondary.getDB(jsTestName()), coll.getName());
    assertGoodKnobOnlyInExplain(secondaryQsutils, query);

    fp.off();
    qsutils.removeAllQuerySettings();
    rst.stopSet();
}

function runGoodMongosBadConfigsvrTest() {
    const st = new ShardingTest({shards: 1, mongos: 1, config: 1});
    const db = st.s.getDB(jsTestName());
    const coll = assertDropAndRecreateCollection(db, jsTestName());
    const qsutils = new QuerySettingsUtils(db, coll.getName());
    qsutils.removeAllQuerySettings();

    const query = qsutils.makeFindQueryInstance({filter: {a: 1}});

    // Arm the failpoint on the configsvr only; mongos parses/validates the command successfully
    // and forwards it, but the configsvr fails to (re-)validate the offending knob.
    const fp = configureFailPoint(st.configRS.getPrimary(), kFailPointName, {name: kBadKnobName});

    assert.commandFailed(db.adminCommand({setQuerySettings: query, settings: makeSettings()}));
    qsutils.assertQueryShapeConfiguration([]);

    fp.off();
    st.stop();
}

function runGoodMongosBadShardTest() {
    const st = new ShardingTest({shards: 1, mongos: 1});
    const db = st.s.getDB(jsTestName());
    const coll = assertDropAndRecreateCollection(db, jsTestName());
    assert.commandWorked(coll.insert({a: 1}));
    const qsutils = new QuerySettingsUtils(db, coll.getName());
    qsutils.removeAllQuerySettings();

    const query = qsutils.makeFindQueryInstance({filter: {a: 1}});

    // Arm the failpoint on the shard primary only; mongos and the configsvr accept and persist
    // the setting, but the shard fails to re-parse the offending knob locally.
    const shardPrimary = st.rs0.getPrimary();
    const fp = configureFailPoint(shardPrimary, kFailPointName, {name: kBadKnobName});

    assert.commandWorked(db.adminCommand({setQuerySettings: query, settings: makeSettings()}));

    // The cluster-parameter-stored configuration (as accepted by mongos/configsvr) has both
    // knobs; the explain-based check below routes through mongos to the shard, which embeds this
    // full value into the outgoing command and gets back the locally-stripped view.
    qsutils.assertQueryShapeConfiguration(
        [qsutils.makeQueryShapeConfiguration(makeSettings(), query)],
        /* shouldRunExplain */ false,
    );

    // Run explain through mongos (not directly against the shard) to confirm the shard answers
    // with only the valid knob applied, whether that comes from its own already-stripped
    // cluster-parameter storage or from re-parsing mongos's forwarded value.
    assertGoodKnobOnlyInExplain(qsutils, query);

    // The shard's own cluster-parameter application already logged and stripped the offending
    // knob when it applied the querySettings cluster parameter; confirm that happened rather than
    // the node crashing.
    assert(
        checkLog.checkContainsOnceJson(shardPrimary, kClusterParameterApplyLogId, {
            reasons: (reasons) => reasons.some((r) => r.includes(kBadKnobName)),
        }),
    );

    // The query must keep executing even though the shard stripped the bad knob locally.
    assert.eq(coll.find({a: 1}).itcount(), 1);

    fp.off();
    qsutils.removeAllQuerySettings();
    st.stop();
}

runGoodPrimaryBadSecondaryTest();
runGoodMongosBadConfigsvrTest();
runGoodMongosBadShardTest();
