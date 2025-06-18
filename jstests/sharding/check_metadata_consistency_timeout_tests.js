/**
 * Checks that the checkMetadataConsistency command times out as expected after grabbing a database
 * DDL lock and setting the timeout parameter.
 *
 * @tags: [
 *   # The parameter is not currently available in older versions.
 *   multiversion_incompatible
 * ]
 */
(function() {
'use strict';

load('jstests/libs/fail_point_util.js');

const kDbName = 'testDB';
const kCollName = 'coll';
const kNss = kDbName + '.' + kCollName;
const st = new ShardingTest({shards: 1});

// Creates a thread that expects checkMetadataConsistency to fail with the error passed as argument.
function createCMCThread({host, dbName, error, dbMetadataLockMaxTimeMS, maxTimeMS}) {
    return new Thread(function(host, dbName, error, dbMetadataLockMaxTimeMS, maxTimeMS) {
        let mongos = new Mongo(host);
        let command = {checkMetadataConsistency: 1};
        if (dbMetadataLockMaxTimeMS > -1) {
            command['dbMetadataLockMaxTimeMS'] = dbMetadataLockMaxTimeMS;
        }
        if (maxTimeMS > -1) {
            command['maxTimeMS'] = maxTimeMS;
        }
        const checkMetadataResult = mongos.getDB(dbName).runCommand(command);
        assert.commandFailedWithCode(checkMetadataResult, error);
    }, host, dbName, error, dbMetadataLockMaxTimeMS, maxTimeMS);
}

function testCMCCommandWithFailpoint(threadParams, failpoint) {
    let asyncCMC = createCMCThread(threadParams);
    const blockCMCPoint = configureFailPoint(st.getPrimaryShard(kDbName), failpoint);
    asyncCMC.start();
    blockCMCPoint.wait();
    // Wait enough time until the timeout happens.
    sleep(1000);
    // Turn off failpoint, it will not be interrupted otherwise.
    blockCMCPoint.off();
    asyncCMC.join();
}

function testCMCCommandWithAsyncDrop(threadParams) {
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
    // Wait enough time until the timeout happens.
    sleep(1000);
    asyncCMC.join();
    blockDDLFailPoint.off();
    asyncDropDatabase.join();
    assert.commandWorked(st.s.adminCommand({shardCollection: kNss, key: {_id: 1}}));
}

assert.commandWorked(st.s.adminCommand({shardCollection: kNss, key: {_id: 1}}));

{
    jsTestLog('Checks that maxTimeMS works when dealing with database lock contention');
    testCMCCommandWithAsyncDrop({
        host: st.s.host,
        dbName: 'admin',
        error: ErrorCodes.MaxTimeMSExpired,
        dbMetadataLockMaxTimeMS: -1,
        maxTimeMS: 100
    });
}
{
    jsTestLog(
        'Checks that dbMetadataLockMaxTimeMS works when dealing with database lock contention');
    testCMCCommandWithAsyncDrop({
        host: st.s.host,
        dbName: 'admin',
        error: 9944001,
        dbMetadataLockMaxTimeMS: 100,
        maxTimeMS: -1
    });
}
{
    jsTestLog(
        'Checks that dbMetadataLockMaxTimeMS does not cover up other ExceededTimedLimit errors');
    testCMCCommandWithFailpoint({
        host: st.s.host,
        dbName: 'admin',
        error: ErrorCodes.ExceededTimeLimit,
        dbMetadataLockMaxTimeMS: 10000,
        maxTimeMS: -1
    },
                                'throwExceededTimeLimitOnCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog('Checks that the dbMetadataLockMaxTimeMS timeout fails with expected error');
    testCMCCommandWithFailpoint({
        host: st.s.host,
        dbName: 'admin',
        error: 9944001,
        dbMetadataLockMaxTimeMS: 100,
        maxTimeMS: -1
    },
                                'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the dbMetadataLockMaxTimeMS timeout fails with expected error when used with maxTimeMS');
    testCMCCommandWithFailpoint({
        host: st.s.host,
        dbName: 'admin',
        error: 9944001,
        dbMetadataLockMaxTimeMS: 100,
        maxTimeMS: 10000
    },
                                'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the maxTimeMS timeout fails with expected error when used with dbMetadataLockMaxTimeMS');
    testCMCCommandWithFailpoint({
        host: st.s.host,
        dbName: 'admin',
        error: ErrorCodes.MaxTimeMSExpired,
        dbMetadataLockMaxTimeMS: 10000,
        maxTimeMS: 100
    },
                                'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that maxTimeMS works when dealing with database lock contention for a specific db');
    testCMCCommandWithAsyncDrop({
        host: st.s.host,
        dbName: kDbName,
        error: ErrorCodes.MaxTimeMSExpired,
        dbMetadataLockMaxTimeMS: -1,
        maxTimeMS: 300
    });
}
{
    jsTestLog(
        'Checks that dbMetadataLockMaxTimeMS works when dealing with database lock contention for a specific db');
    testCMCCommandWithAsyncDrop({
        host: st.s.host,
        dbName: kDbName,
        error: 9944001,
        dbMetadataLockMaxTimeMS: 100,
        maxTimeMS: -1
    });
}
{
    jsTestLog(
        'Checks that dbMetadataLockMaxTimeMS does not cover up other ExceededTimedLimit errors for a specific db');
    testCMCCommandWithFailpoint({
        host: st.s.host,
        dbName: kDbName,
        error: ErrorCodes.ExceededTimeLimit,
        dbMetadataLockMaxTimeMS: 10000,
        maxTimeMS: -1
    },
                                'throwExceededTimeLimitOnCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the dbMetadataLockMaxTimeMS timeout fails with expected error for a specific db');
    testCMCCommandWithFailpoint({
        host: st.s.host,
        dbName: kDbName,
        error: 9944001,
        dbMetadataLockMaxTimeMS: 100,
        maxTimeMS: -1
    },
                                'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the dbMetadataLockMaxTimeMS timeout fails with expected error when used with maxTimeMS for a specific db');
    testCMCCommandWithFailpoint({
        host: st.s.host,
        dbName: kDbName,
        error: 9944001,
        dbMetadataLockMaxTimeMS: 100,
        maxTimeMS: 10000
    },
                                'hangShardCheckMetadataBeforeEstablishCursors');
}
{
    jsTestLog(
        'Checks that the maxTimeMS timeout fails with expected error when used with dbMetadataLockMaxTimeMS for a specific db');
    testCMCCommandWithFailpoint({
        host: st.s.host,
        dbName: kDbName,
        error: ErrorCodes.MaxTimeMSExpired,
        dbMetadataLockMaxTimeMS: 10000,
        maxTimeMS: 100
    },
                                'hangShardCheckMetadataBeforeEstablishCursors');
}
st.stop();
})();
