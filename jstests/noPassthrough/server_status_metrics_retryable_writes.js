/**
 * Tests the retryable write counts in mongos and mongod serverStatus output
 * for a replica set and a sharded cluster.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

function assertServerStatus(expectedServerStatusMetrics, externalConn, internalConn) {
    const externalServerStatus = assert.commandWorked(externalConn.adminCommand({serverStatus: 1}));
    assert.eq(expectedServerStatusMetrics['external']['internalRetryableWriteCount'],
              externalServerStatus.metrics.query.internalRetryableWriteCount);
    assert.eq(expectedServerStatusMetrics['external']['externalRetryableWriteCount'],
              externalServerStatus.metrics.query.externalRetryableWriteCount);
    assert.eq(expectedServerStatusMetrics['external']['retryableInternalTransactionCount'],
              externalServerStatus.metrics.query.retryableInternalTransactionCount);
    if (internalConn) {
        const internalServerStatus =
            assert.commandWorked(internalConn.adminCommand({serverStatus: 1}));
        assert.eq(expectedServerStatusMetrics['internal']['internalRetryableWriteCount'],
                  internalServerStatus.metrics.query.internalRetryableWriteCount);
        assert.eq(expectedServerStatusMetrics['internal']['externalRetryableWriteCount'],
                  internalServerStatus.metrics.query.externalRetryableWriteCount);
        assert.eq(expectedServerStatusMetrics['internal']['retryableInternalTransactionCount'],
                  internalServerStatus.metrics.query.retryableInternalTransactionCount);
    }
}

function runTest(externalConn, internalConn = undefined) {
    // Setting up
    const dbName = "test";
    const testDB = externalConn.getDB(dbName);
    const coll = testDB.coll;

    assert.commandWorked(coll.insert({x: -1, _id: -1}));
    assert.commandWorked(coll.insert({x: 1, _id: 1}));

    let txnNumber = NumberLong(1);
    const lsid = {id: UUID()};
    const lsidWithUUID = {id: UUID(), txnUUID: UUID()};
    const lsidWithUUIDAndTxnNum = {
        id: UUID(),
        txnUUID: UUID(),
        txnNumber: NumberLong(2),
    };
    const update = {q: {_id: 2}, u: {$inc: {counter: 1}}};

    // Check initial metrics.
    let expectedServerStatusMetrics = {};
    expectedServerStatusMetrics['external'] = {
        'internalRetryableWriteCount': 0,
        'externalRetryableWriteCount': 0,
        'retryableInternalTransactionCount': 0
    };
    if (internalConn) {
        const internalServerStatus =
            assert.commandWorked(internalConn.adminCommand({serverStatus: 1}));
        expectedServerStatusMetrics['internal'] = {
            'internalRetryableWriteCount':
                internalServerStatus.metrics.query.internalRetryableWriteCount,
            'externalRetryableWriteCount': 0,
            'retryableInternalTransactionCount':
                internalServerStatus.metrics.query.retryableInternalTransactionCount
        };
    }

    assertServerStatus(expectedServerStatusMetrics, externalConn, internalConn);

    // Ordinary write without session
    assert.commandWorked(testDB.runCommand({update: coll.getName(), updates: [update]}));

    assertServerStatus(expectedServerStatusMetrics, externalConn, internalConn);

    // Non-retryable write with session
    assert.commandWorked(
        testDB.runCommand({update: coll.getName(), updates: [update], lsid: lsid}));

    assertServerStatus(expectedServerStatusMetrics, externalConn, internalConn);

    // Retryable write with session
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [update],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber++)
    }));

    expectedServerStatusMetrics['external']['externalRetryableWriteCount'] += 1;
    if (internalConn) {
        expectedServerStatusMetrics['internal']['internalRetryableWriteCount'] += 1;
    }

    assertServerStatus(expectedServerStatusMetrics, externalConn, internalConn);

    // Write with session and in transaction, lsid with id.
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [update],
        lsid: lsid,
        txnNumber: NumberLong(txnNumber++),
        startTransaction: true,
        autocommit: false
    }));

    assertServerStatus(expectedServerStatusMetrics, externalConn, internalConn);

    // Write with session and in transaction, lsid with id and txnUUID.
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [update],
        lsid: lsidWithUUID,
        txnNumber: NumberLong(txnNumber++),
        startTransaction: true,
        autocommit: false
    }));

    assertServerStatus(expectedServerStatusMetrics, externalConn, internalConn);

    // Write with session and in transaction, lsid with id, txnUUID and txnNumber.
    assert.commandWorked(testDB.runCommand({
        update: coll.getName(),
        updates: [update],
        lsid: lsidWithUUIDAndTxnNum,
        txnNumber: NumberLong(txnNumber++),
        startTransaction: true,
        autocommit: false
    }));

    expectedServerStatusMetrics['external']['retryableInternalTransactionCount'] += 1;
    if (internalConn) {
        expectedServerStatusMetrics['internal']['retryableInternalTransactionCount'] += 1;
    }

    assertServerStatus(expectedServerStatusMetrics, externalConn, internalConn);
}

jsTest.log(
    "Tests the retryable write counts in mongos and mongod serverStatus output for a sharded cluster.");
{
    const st = new ShardingTest({shards: 1, mongos: 1});
    const mongos = st.s0;
    const shard0Primary = st.rs0.getPrimary();

    runTest(mongos, shard0Primary);

    st.stop();
}

jsTest.log("Tests the retryable write counts in mongod serverStatus output for a replica set.");
{
    const rt = new ReplSetTest({name: "server_status_repl", nodes: 1});
    rt.startSet();
    rt.initiate();
    const primary = rt.getPrimary();

    runTest(primary);

    rt.stopSet();
}
