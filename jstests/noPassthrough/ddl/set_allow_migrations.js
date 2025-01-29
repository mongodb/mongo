/**
 * Tests basic functionality of the setAllowMigrations command.
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2});

// Setup two sharded collections; one of them will be targeted by setAllowMigrations commands.
const dbName = 'testDB';
const collName = 'testColl';
const nss = dbName + '.' + collName;
const anotherCollName = 'testColl2';
const anotherNss = dbName + '.' + anotherCollName;

const primaryShardName = st.shard0.shardName;
const recipientShardName = st.shard1.shardName;
const shardKey = {
    k: 1
};
const splitPoint = {
    k: 0
};

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: primaryShardName}));

for (let cn of [collName, anotherCollName]) {
    st.shardColl(cn, shardKey, splitPoint /*split*/, false /*move*/, dbName);
    const fullName = dbName + '.' + cn;
    // Ensure that each chunk that will be targeted during the test cases is initially located on
    // the primary shard.
    assert.commandWorked(
        st.s.adminCommand({moveChunk: fullName, find: splitPoint, to: primaryShardName}));
}

jsTest.log('{setAllowMigrations: false} causes inflight migrations to abort');
{
    const donorPrimary = st.rs0.getPrimary();
    let migrationHangsRightBeforeCommitting =
        configureFailPoint(donorPrimary, "moveChunkHangAtStep5");

    let awaitShell = startParallelShell(
        funWithArgs((nss, matchingChunk, recipient) => {
            assert.commandFailedWithCode(
                db.adminCommand({moveChunk: nss, find: matchingChunk, to: recipient}),
                ErrorCodes.ConflictingOperationInProgress);
        }, nss, splitPoint, recipientShardName), st.s.port);

    migrationHangsRightBeforeCommitting.wait();

    assert.commandWorked(st.s.adminCommand({setAllowMigrations: nss, allowMigrations: false}));

    migrationHangsRightBeforeCommitting.off();

    awaitShell();
}

jsTest.log('{setAllowMigrations: false} causes abort new migrations to abort');
{
    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: nss, find: splitPoint, to: recipientShardName}),
        ErrorCodes.ConflictingOperationInProgress);
}

jsTest.log('{setAllowMigrations: false} does not affect other namespaces');
{
    assert.commandWorked(
        st.s.adminCommand({moveChunk: anotherNss, find: splitPoint, to: recipientShardName}));
}

jsTest.log('{setAllowMigrations: true} allows new migrations to be committed');
{
    assert.commandWorked(st.s.adminCommand({setAllowMigrations: nss, allowMigrations: true}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: nss, find: splitPoint, to: recipientShardName}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: nss, find: splitPoint, to: primaryShardName}));
}

st.stop();
