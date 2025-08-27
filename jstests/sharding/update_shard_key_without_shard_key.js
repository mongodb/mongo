/**
 * Tests to validate functionality of update command when the update operation modifies the
 * shard key.
 *
 * @tags: [
 *  cannot_run_during_upgrade_downgrade,
 *  featureFlagUpdateOneWithoutShardKey,
 *  multiversion_incompatible,
 *  requires_fcv_71,
 *  requires_sharding,
 *  uses_multi_shard_transaction,
 *  uses_transactions,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {execCtxTypes} from "jstests/noPassthrough/rs_endpoint/lib/util.js";
import {CreateShardedCollectionUtil} from "jstests/sharding/libs/create_sharded_collection_util.js";
import {makeCommitTransactionCmdObj} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const st = new ShardingTest({shards: 4});
const mongos = st.s0;
const db = mongos.getDB(jsTestName());

const coll = db.coll;
coll.drop();

CreateShardedCollectionUtil.shardCollectionWithChunks(coll, {x: 1}, [
    {min: {x: MinKey}, max: {x: -100}, shard: st.shard0.shardName},
    {min: {x: -100}, max: {x: 0}, shard: st.shard1.shardName},
    {min: {x: 0}, max: {x: 100}, shard: st.shard2.shardName},
    {min: {x: 100}, max: {x: MaxKey}, shard: st.shard3.shardName},
]);

const session = st.s.startSession({retryWrites: true});
const lsid = session.getSessionId();

let nextTxnNum = 1;
let useSession = false;
let useRetryableWrites = false;
let useTransactions = false;
let useFindAndModify = false;
let doUpsert = false;

function runTest(docs, expectedResults, filter, updateMods) {
    const expectError = !Array.isArray(expectedResults);
    const expectedErrorCode = expectError ? expectedResults : undefined;

    assert.commandWorked(coll.insert(docs));

    let cmdObj = null;

    if (useFindAndModify) {
        cmdObj = {
            findAndModify: coll.getName(),
            query: filter,
            update: updateMods,
            upsert: doUpsert,
        };
    } else {
        cmdObj = {
            update: coll.getName(),
            updates: [{q: filter, u: updateMods, upsert: doUpsert}],
        };
    }

    let txnNumber = null;

    if (useSession) {
        cmdObj.lsid = lsid;

        if (useRetryableWrites || useTransactions) {
            txnNumber = nextTxnNum++;
            cmdObj.txnNumber = NumberLong(txnNumber);
        }

        if (useTransactions) {
            cmdObj.startTransaction = true;
            cmdObj.autocommit = false;
        }
    }

    const ret = db.runCommand(cmdObj);

    if (!expectError) {
        assert.commandWorked(ret);

        if (useTransactions) {
            assert.commandWorked(db.adminCommand(makeCommitTransactionCmdObj(lsid, txnNumber)));
        }

        let results = coll.find().toArray().sort(bsonWoCompare);
        assert.eq(results, expectedResults);
    } else {
        assert.commandFailedWithCode(ret, expectedErrorCode);
    }

    assert.commandWorked(coll.remove({}));
};

function runAllTestsForConfigAndExecCtx(config, execCtxType) {
    useSession = (execCtxType != execCtxTypes.kNoSession);
    useRetryableWrites = (execCtxType == execCtxTypes.kRetryableWrite);
    useTransactions = (execCtxType == execCtxTypes.kTransaction);

    useFindAndModify = config.hasOwnProperty("findAndModify") ? config.findAndModify : false;
    doUpsert = config.hasOwnProperty("upsert") ? config.upsert : false;

    const doReplacementUpdate =
        config.hasOwnProperty("replacementUpdate") ? config.replacementUpdate : false;

    const docs = [{_id: 1, x: -1, y: 1, z: 1}];
    const docsUpdated = [{_id: 1, y: 1, z: -1}];
    const mods = doReplacementUpdate ? {y: 1, z: -1} : {$set: {z: -1}, $unset: {x: 1}};

    const expectSuccess = (useRetryableWrites || useTransactions);
    const expectedResults = expectSuccess ? docsUpdated : ErrorCodes.IllegalOperation;

    runTest(docs, expectedResults, {x: -1}, mods);
    runTest(docs, expectedResults, {_id: 1, x: -1}, mods);
    runTest(docs, expectedResults, {y: 1}, mods);
    runTest(docs, expectedResults, {x: {$gte: -2, $lte: 1}}, mods);
    runTest(docs, expectedResults, {x: {$gte: -2, $lte: -1}}, mods);
    runTest(docs, expectedResults, {_id: 1}, mods);
    runTest(docs, expectedResults, {x: {$gte: -2, $lte: 1}, _id: 1}, mods);
    runTest(docs, expectedResults, {x: {$gte: -2, $lte: -1}, _id: 1}, mods);
}

function runAllTestsForConfig(config) {
    runAllTestsForConfigAndExecCtx(config, execCtxTypes.kNoSession);
    runAllTestsForConfigAndExecCtx(config, execCtxTypes.kNonRetryableWrite);
    runAllTestsForConfigAndExecCtx(config, execCtxTypes.kRetryableWrite);
    runAllTestsForConfigAndExecCtx(config, execCtxTypes.kTransaction);
}

// Test the "update" command.
runAllTestsForConfig({});
runAllTestsForConfig({upsert: true});
runAllTestsForConfig({replacementUpdate: true});
runAllTestsForConfig({upsert: true, replacementUpdate: true});

// Test the "findAndModify" command.
runAllTestsForConfig({findAndModify: true});
runAllTestsForConfig({findAndModify: true, upsert: true});
runAllTestsForConfig({findAndModify: true, replacementUpdate: true});
runAllTestsForConfig({findAndModify: true, upsert: true, replacementUpdate: true});

session.endSession();

st.stop();
