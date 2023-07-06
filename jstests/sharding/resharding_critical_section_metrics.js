/**
 * Tests that resharding reads and writes during critical section metrics are incremented.
 *
 * @tags: [
 *   requires_fcv_61
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ReshardingTest} from "jstests/sharding/libs/resharding_test_fixture.js";

const reshardingTest = new ReshardingTest();
reshardingTest.setup();

const donorName = reshardingTest.donorShardNames[0];
const recipientName = reshardingTest.recipientShardNames[0];
const donorShard = reshardingTest.getReplSetForShard(donorName).getPrimary();
const sourceCollection = reshardingTest.createShardedCollection({
    ns: 'reshardingDb.coll',
    shardKeyPattern: {oldKey: 1},
    chunks: [
        {min: {oldKey: MinKey}, max: {oldKey: MaxKey}, shard: donorName},
    ]
});
const mongos = sourceCollection.getMongo();

const kWritesDuringCriticalSection = 'countWritesDuringCriticalSection';
const kReadsDuringCriticalSection = 'countReadsDuringCriticalSection';

function attemptFromParallelShell(fn) {
    const join = startParallelShell(funWithArgs((fn) => {
                                        fn(db.getSiblingDB('reshardingDb').coll);
                                    }, fn), mongos.port);
    return join;
}

function attemptWriteFromParallelShell() {
    return attemptFromParallelShell((coll) => {
        assert.commandWorked(coll.insert({_id: 0, oldKey: 0, newKey: 0}));
    });
}

function attemptReadFromParallelShell() {
    return attemptFromParallelShell((coll) => {
        coll.find({}).toArray();
    });
}

function getActiveSectionMetric(fieldName) {
    const stats = donorShard.getDB('admin').serverStatus({});
    const active = stats.shardingStatistics.resharding.active;
    return active[fieldName];
}

function assertIncrementsActiveSectionMetricSoon(fn, metricFieldName) {
    const before = getActiveSectionMetric(metricFieldName);
    fn();
    assert.soon(() => {
        const after = getActiveSectionMetric(metricFieldName);
        return after > before;
    });
}

const hangWhileBlockingReads =
    configureFailPoint(donorShard, "reshardingPauseDonorAfterBlockingReads");

let waitForWrite;
let waitForRead;

reshardingTest.withReshardingInBackground({
    newShardKeyPattern: {newKey: 1},
    newChunks: [{min: {newKey: MinKey}, max: {newKey: MaxKey}, shard: recipientName}],
},
                                          () => {},
                                          {
                                              postDecisionPersistedFn: () => {
                                                  hangWhileBlockingReads.wait();
                                                  assertIncrementsActiveSectionMetricSoon(() => {
                                                      waitForWrite =
                                                          attemptWriteFromParallelShell();
                                                  }, kWritesDuringCriticalSection);
                                                  assertIncrementsActiveSectionMetricSoon(() => {
                                                      waitForRead = attemptReadFromParallelShell();
                                                  }, kReadsDuringCriticalSection);
                                                  hangWhileBlockingReads.off();
                                              }
                                          });

waitForWrite();
waitForRead();

reshardingTest.teardown();
