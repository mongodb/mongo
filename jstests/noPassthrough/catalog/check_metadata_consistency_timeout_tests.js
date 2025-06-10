/**
 * Checks that the checkMetadataConsistency command times out as expected after grabbing a database
 * DDL lock and setting the timeout parameter.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kDbName = 'testDB';
const kCollName = 'coll';
const kNss = kDbName + '.' + kCollName;
const st = new ShardingTest({shards: 1});

// Creates a thread that expects checkMetadataConsistency to fail with the error passed as argument.
function createCMCThread({host, dbName, dbMetadataLockMaxTimeMS, maxTimeMS}) {
    return new Thread(function(host, dbName, dbMetadataLockMaxTimeMS, maxTimeMS) {
        let mongos = new Mongo(host);
        let command = {checkMetadataConsistency: 1};
        if (dbMetadataLockMaxTimeMS > -1) {
            command['dbMetadataLockMaxTimeMS'] = dbMetadataLockMaxTimeMS;
        }
        if (maxTimeMS > -1) {
            command['maxTimeMS'] = maxTimeMS;
        }
        return mongos.getDB(dbName).runCommand(command);
    }, host, dbName, dbMetadataLockMaxTimeMS, maxTimeMS);
}

function testCMCCommandWithFailpoint(threadParams, expectedError, failpoint) {
    let maxTimeMS = threadParams.maxTimeMS;
    const isMaxTimeMSLessThanLockMaxTimeMS = maxTimeMS < threadParams.dbMetadataLockMaxTimeMS;
    const maxRetries = maxTimeMS > -1 ? 5 : 1;
    let retries = 0;
    // On slow variants, the `checkMetadataConsistency` command may timeout with
    // `ErrorCodes.MaxTimeMSExpired` before the failpoint can be reached.
    // To address this, we retry the CMC command, doubling the `maxTimeMS`
    // value on each iteration.
    while (retries < maxRetries) {
        assert.eq(
            isMaxTimeMSLessThanLockMaxTimeMS,
            maxTimeMS < threadParams.dbMetadataLockMaxTimeMS,
            'The relationship between the initially provided maxTimeMS and dbMetadataLockMaxTimeMS ' +
                'parameters must remain consistent across retries.');
        let asyncCMC = createCMCThread({...threadParams, maxTimeMS});
        const blockCMCPoint = configureFailPoint(st.getPrimaryShard(kDbName), failpoint);
        asyncCMC.start();
        // Wait 60s for the failpoint to be reached
        if (blockCMCPoint.waitWithTimeout(60 * 1000)) {
            // Wait enough time until the CMC timeout happens.
            sleep(1000);
            blockCMCPoint.off();
            asyncCMC.join();
            assert.commandFailedWithCode(asyncCMC.returnData(), expectedError);
            break;
        }
        blockCMCPoint.off();
        asyncCMC.join();
        assert.commandFailedWithCode(asyncCMC.returnData(), ErrorCodes.MaxTimeMSExpired);
        maxTimeMS *= 2;
        retries++;
    }
    assert.lt(retries,
              maxRetries,
              `Failed to reach ${failpoint} failpoint after ${maxRetries} attempts.`);
}

function testCMCCommandWithAsyncDrop(threadParams, expectedError) {
    const blockDDLFailPoint =
        configureFailPoint(st.getPrimaryShard(kDbName), 'hangBeforeRunningCoordinatorInstance');
    // Launch drop database operation and use a failpoint to make it block after taking the DDL lock
    let asyncDropDatabase = new Thread(function(host, dbName) {
        let conn = new Mongo(host);
        const res = conn.getDB(dbName).runCommand({dropDatabase: 1});
        assert.commandWorked(res);
    }, st.s.host, kDbName);
    asyncDropDatabase.start();
    blockDDLFailPoint.wait();
    let asyncCMC = createCMCThread(threadParams);
    asyncCMC.start();
    // Wait enough time until the CMC timeout happens.
    sleep(1000);
    asyncCMC.join();
    assert.commandFailedWithCode(asyncCMC.returnData(), expectedError);
    blockDDLFailPoint.off();
    asyncDropDatabase.join();
    assert.commandWorked(st.s.adminCommand({shardCollection: kNss, key: {_id: 1}}));
}

assert.commandWorked(st.s.adminCommand({shardCollection: kNss, key: {_id: 1}}));

{
    jsTestLog('Checks that maxTimeMS works when dealing with database lock contention');
    testCMCCommandWithAsyncDrop(
        {host: st.s.host, dbName: 'admin', dbMetadataLockMaxTimeMS: -1, maxTimeMS: 100},
        ErrorCodes.MaxTimeMSExpired);
}
{
    jsTestLog(
        'Checks that dbMetadataLockMaxTimeMS works when dealing with database lock contention');
    testCMCCommandWithAsyncDrop(
        {host: st.s.host, dbName: 'admin', dbMetadataLockMaxTimeMS: 100, maxTimeMS: -1}, 9944001);
}
{
    jsTestLog(
        'Checks that dbMetadataLockMaxTimeMS does not cover up other ExceededTimedLimit errors');
    testCMCCommandWithFailpoint(
        {host: st.s.host, dbName: 'admin', dbMetadataLockMaxTimeMS: 10000, maxTimeMS: -1},
        ErrorCodes.ExceededTimeLimit,
        'throwExceededTimeLimitOnCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog('Checks that the dbMetadataLockMaxTimeMS timeout fails with expected error');
    testCMCCommandWithFailpoint(
        {host: st.s.host, dbName: 'admin', dbMetadataLockMaxTimeMS: 100, maxTimeMS: -1},
        9944001,
        'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the dbMetadataLockMaxTimeMS timeout fails with expected error when used with maxTimeMS');
    testCMCCommandWithFailpoint(
        {host: st.s.host, dbName: 'admin', dbMetadataLockMaxTimeMS: 100, maxTimeMS: 10000},
        9944001,
        'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the maxTimeMS timeout fails with expected error when used with dbMetadataLockMaxTimeMS');
    testCMCCommandWithFailpoint(
        {host: st.s.host, dbName: 'admin', dbMetadataLockMaxTimeMS: 10000, maxTimeMS: 100},
        ErrorCodes.MaxTimeMSExpired,
        'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that maxTimeMS works when dealing with database lock contention for a specific db');
    testCMCCommandWithAsyncDrop(
        {host: st.s.host, dbName: kDbName, dbMetadataLockMaxTimeMS: -1, maxTimeMS: 300},
        ErrorCodes.MaxTimeMSExpired);
}
{
    jsTestLog(
        'Checks that dbMetadataLockMaxTimeMS works when dealing with database lock contention for a specific db');
    testCMCCommandWithAsyncDrop(
        {host: st.s.host, dbName: kDbName, dbMetadataLockMaxTimeMS: 100, maxTimeMS: -1}, 9944001);
}
{
    jsTestLog(
        'Checks that dbMetadataLockMaxTimeMS does not cover up other ExceededTimedLimit errors for a specific db');
    testCMCCommandWithFailpoint(
        {host: st.s.host, dbName: kDbName, dbMetadataLockMaxTimeMS: 10000, maxTimeMS: -1},
        ErrorCodes.ExceededTimeLimit,
        'throwExceededTimeLimitOnCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the dbMetadataLockMaxTimeMS timeout fails with expected error for a specific db');
    testCMCCommandWithFailpoint(
        {host: st.s.host, dbName: kDbName, dbMetadataLockMaxTimeMS: 100, maxTimeMS: -1},
        9944001,
        'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the dbMetadataLockMaxTimeMS timeout fails with expected error when used with maxTimeMS for a specific db');
    testCMCCommandWithFailpoint(
        {host: st.s.host, dbName: kDbName, dbMetadataLockMaxTimeMS: 100, maxTimeMS: 10000},
        9944001,
        'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the maxTimeMS timeout fails with expected error when used with dbMetadataLockMaxTimeMS for a specific db');
    testCMCCommandWithFailpoint(
        {host: st.s.host, dbName: kDbName, dbMetadataLockMaxTimeMS: 10000, maxTimeMS: 100},
        ErrorCodes.MaxTimeMSExpired,
        'hangShardCheckMetadataBeforeEstablishCursors');
}
st.stop();
