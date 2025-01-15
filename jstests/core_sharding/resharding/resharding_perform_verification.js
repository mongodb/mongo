/**
 * Tests that resharding commands support 'performVerification'. Also tests that when
 * 'performVerification' is unspecified, if the featureFlagReshardingVerification is enabled when
 * the command started, it would get set to true; otherwise, it would not get set.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   assumes_unsharded_collection,
 *   requires_2_or_more_shards,
 *   # This test expects the FCV to remain the same the entire time (i.e. the feature flag to always
 *   # be enabled or disabled). Also, FCV upgrade and downgrade aborts resharding operations.
 *   cannot_run_during_upgrade_downgrade,
 *   # This test sets a failpoint on the primary so it is not compatible with stepdowns.
 *   does_not_support_stepdowns,
 * ]
 */

import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {
    getShardNamesForCollection,
} from "jstests/sharding/libs/sharding_util.js";

const topology = DiscoverTopology.findConnectedNodes(db);
const isVerificationEnabled = FeatureFlagUtil.isEnabled(db, "ReshardingVerification");

const dbName = jsTestName();
const testDB = db.getSiblingDB(dbName);

let collectionNum = 0;
function getTestCollectionName() {
    return "testColl" + collectionNum++;
}

function makeDocuments(numDocs) {
    const docs = [];
    for (let i = 1; i <= numDocs / 2; i++) {
        docs.push({x: -i});
        docs.push({x: i});
    }
    return docs;
}

function getNonOwningShardName(dbName, collName) {
    const shardNames = getShardNamesForCollection(db.getMongo(), dbName, collName);

    for (let shardName in topology.shards) {
        if (!shardNames.has(shardName)) {
            return shardName;
        }
    }
    throw "Could not find a shard other than " + shardNames;
}

function runMoveCollection(host, ns, toShard, performVerification, countDownLatch) {
    const mongos = new Mongo(host);
    const cmdObj = {moveCollection: ns, toShard};
    if (performVerification !== null) {
        cmdObj.performVerification = performVerification;
    }
    const res = mongos.adminCommand(cmdObj);
    countDownLatch.countDown();
    return res;
}

function runReshardCollection(host, ns, key, performVerification, countDownLatch) {
    const mongos = new Mongo(host);
    const cmdObj = {reshardCollection: ns, key};
    if (performVerification !== null) {
        cmdObj.performVerification = performVerification;
    }
    const res = mongos.adminCommand(cmdObj);
    countDownLatch.countDown();

    return res;
}

function runUnshardCollection(host, ns, toShard, performVerification, countDownLatch) {
    const mongos = new Mongo(host);
    const cmdObj = {unshardCollection: ns, toShard};
    if (performVerification !== null) {
        cmdObj.performVerification = performVerification;
    }
    const res = mongos.adminCommand(cmdObj);
    countDownLatch.countDown();
    return res;
}

function pauseReshardingBeforeDecisionPersisted(topology) {
    const node = new Mongo(topology.configsvr.primary);
    return configureFailPoint(node, "reshardingPauseCoordinatorBeforeDecisionPersisted");
}

function waitForFailPointOrCountDownLatch(fp, countDownLatch) {
    let enteredReshardFp = false;
    assert.soonNoExcept(() => {
        try {
            fp.wait({maxTimeMS: 100});
        } catch (e) {
            if (e.code == ErrorCodes.MaxTimeMSExpired) {
                if (countDownLatch.getCount() == 0) {
                    return true;
                }
                return false;
            }
            throw e;
        }
        enteredReshardFp = true;
        return true;
    });
    return enteredReshardFp;
}

function validateStateDocuments(topology, performVerification) {
    const configRSPrimary = new Mongo(topology.configsvr.primary);
    const coordinatorDoc = configRSPrimary.getCollection("config.reshardingOperations").findOne();

    if (performVerification === null && isVerificationEnabled) {
        // The command didn't specify 'performVerification'. If the feature flag was enabled when
        // the command started, 'performVerification' would get set to true. Otherwise, it would not
        // get set.
        performVerification = true;
    }
    jsTest.log("Validating state documents " +
               tojson({expectedPerformVerification: performVerification}));

    assert.eq(coordinatorDoc.performVerification, performVerification, coordinatorDoc);

    coordinatorDoc.donorShards.forEach(donorEntry => {
        const shardRSPrimary = new Mongo(topology.shards[donorEntry.id].primary);
        const donorDoc =
            shardRSPrimary.getCollection("config.localReshardingOperations.donor").findOne();
        assert.eq(donorDoc.performVerification, performVerification, donorDoc);
    });

    coordinatorDoc.recipientShards.forEach(recipientEntry => {
        const shardRSPrimary = new Mongo(topology.shards[recipientEntry.id].primary);
        const recipientDoc =
            shardRSPrimary.getCollection("config.localReshardingOperations.recipient").findOne();
        assert.eq(recipientDoc.performVerification, performVerification, recipientDoc);
    });
}

function testResharding(thread, countDownLatch, performVerification) {
    // Pause resharding before it commits so we can inspect the state documents.
    const commitFp = pauseReshardingBeforeDecisionPersisted(topology);
    thread.start();
    const enteredCommitFp = waitForFailPointOrCountDownLatch(commitFp, countDownLatch);

    if (enteredCommitFp) {
        validateStateDocuments(topology, performVerification);
        commitFp.off();
        assert.commandWorked(thread.returnData());
    } else {
        const res = thread.returnData();
        if (res.code == ErrorCodes.IDLUnknownField) {
            // This error is expected when this test runs in a mixed version cluster and it
            // specifies 'performVerification', and the resharding command runs on a configsvr or
            // shardsvr node that does not know about the this field.
            assert.neq(performVerification, null, res);
        } else {
            // This error is expected when this test runs in a mixed version cluster and it
            // specifies 'performVerification' to true, and the resharding command runs only on
            // configsvr or shardsvr nodes that know about the this field.
            assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
            assert(res.errmsg.includes("Cannot specify 'performVerification' to true when " +
                                       "featureFlagReshardingVerification is not enabled"),
                   res);
            assert.eq(performVerification, true, res);
            assert.eq(isVerificationEnabled, false);
        }
    }
}

function testReshardCollection(performVerification) {
    const collName = getTestCollectionName();
    const ns = dbName + "." + collName;
    const numDocs = 1000;
    const docs = makeDocuments(numDocs);

    assert.commandWorked(testDB.getCollection(collName).insert(docs));
    assert.commandWorked(db.adminCommand({shardCollection: ns, key: {_id: 1}}));
    assert.commandWorked(db.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(db.adminCommand({
        moveChunk: ns,
        find: {_id: 0},
        to: getNonOwningShardName(dbName, collName),
        _waitForDelete: true
    }));

    jsTest.log("Testing reshardCollection with " + tojson({performVerification}));
    const reshardCountDownLatch = new CountDownLatch(1);
    const reshardThread = new Thread(runReshardCollection,
                                     db.getMongo().host,
                                     ns,
                                     {x: 1} /* key */,
                                     performVerification,
                                     reshardCountDownLatch);
    testResharding(reshardThread, reshardCountDownLatch, performVerification);
}

function testUnshardCollection(performVerification) {
    const collName = getTestCollectionName();
    const ns = dbName + "." + collName;
    const numDocs = 10;
    const docs = makeDocuments(numDocs);

    assert.commandWorked(testDB.getCollection(collName).insert(docs));
    assert.commandWorked(db.adminCommand({shardCollection: ns, key: {_id: 1}}));

    jsTest.log("Testing unshardCollection with " + tojson({performVerification}));
    const unshardCountDownLatch = new CountDownLatch(1);
    const unshardThread = new Thread(runUnshardCollection,
                                     db.getMongo().host,
                                     ns,
                                     getNonOwningShardName(dbName, collName),
                                     performVerification,
                                     unshardCountDownLatch);
    testResharding(unshardThread, unshardCountDownLatch, performVerification);
}

function testMoveCollection(performVerification) {
    const collName = getTestCollectionName();
    const ns = dbName + "." + collName;
    const numDocs = 10;
    const docs = makeDocuments(numDocs);

    assert.commandWorked(testDB.getCollection(collName).insert(docs));

    jsTest.log("Testing moveCollection with " + tojson({performVerification}));
    const moveCountDownLatch = new CountDownLatch(1);
    const moveThread = new Thread(runMoveCollection,
                                  db.getMongo().host,
                                  ns,
                                  getNonOwningShardName(dbName, collName),
                                  performVerification,
                                  moveCountDownLatch);
    testResharding(moveThread, moveCountDownLatch, performVerification);
}

function runTest(performVerification) {
    testReshardCollection(performVerification);
    testUnshardCollection(performVerification);
    testMoveCollection(performVerification);
}

runTest(true /* performVerification */);
runTest(false /* performVerification */);
runTest(null /* performVerification */);
