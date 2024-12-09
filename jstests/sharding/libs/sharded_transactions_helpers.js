import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

export const kSnapshotErrors =
    [ErrorCodes.SnapshotTooOld, ErrorCodes.SnapshotUnavailable, ErrorCodes.StaleChunkHistory];

// List of failpoints in the coordinator's two-phase commit code. The associated data describes how
// many times each failpoint would be hit assuming a 3-participant transaction where one of the
// participants is co-located with the coordinator:
// - numTimesShouldBeHit: N means the failpoint should be hit N times during that phase; for
//   example, if there are two remote participants, the hangWhileTargetingRemoteHost failpoint would
//   be hit two times in the prepare phase.
// - skip: N means turn on the failpoint after the failpoint has been hit N times; it's used to turn
//   on the remote and local targeting failpoints for the prepare and decision phase separately.
export function getCoordinatorFailpoints() {
    const coordinatorFailpointDataArr = [
        {failpoint: "hangBeforeWritingParticipantList", numTimesShouldBeHit: 1},
        {
            // Test targeting remote nodes for prepare
            failpoint: "hangWhileTargetingRemoteHost",
            numTimesShouldBeHit: 2, /* once per remote participant */
            data: {twoPhaseCommitStage: "prepare"}
        },
        {
            // Test targeting local node for prepare
            failpoint: "hangWhileTargetingLocalHost",
            numTimesShouldBeHit: 1,
            data: {twoPhaseCommitStage: "prepare"}
        },
        {failpoint: "hangBeforeWritingDecision", numTimesShouldBeHit: 1},
        {
            // Test targeting remote nodes for decision
            failpoint: "hangWhileTargetingRemoteHost",
            numTimesShouldBeHit: 2, /* once per remote participant */
            data: {twoPhaseCommitStage: "decision"},
        },
        {
            // Test targeting local node for decision
            failpoint: "hangWhileTargetingLocalHost",
            numTimesShouldBeHit: 1,
            data: {twoPhaseCommitStage: "decision"},
        },
        {failpoint: "hangBeforeDeletingCoordinatorDoc", numTimesShouldBeHit: 1},
    ];

    // Return a deep copy of the array, so that the caller is free to modify its contents.
    return coordinatorFailpointDataArr.map(failpoint => Object.assign({}, failpoint));
}

export function setFailCommandOnShards(st, mode, data, numShards) {
    for (let i = 0; i < numShards; i++) {
        const shardConn = st["rs" + i].getPrimary();
        // Sharding tests require failInternalCommands: true, since the mongos appears to mongod to
        // be an internal client.
        assert.commandWorked(shardConn.adminCommand({
            configureFailPoint: "failCommand",
            mode: mode,
            data: {...data, failInternalCommands: true}
        }));
    }
}

export function unsetFailCommandOnEachShard(st, numShards) {
    for (let i = 0; i < numShards; i++) {
        const shardConn = st["rs" + i].getPrimary();
        assert.commandWorked(
            shardConn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
    }
}

export function assertNoSuchTransactionOnAllShards(st, lsid, txnNumber) {
    st._rs.forEach(function(rs) {
        assertNoSuchTransactionOnConn(rs.test.getPrimary(), lsid, txnNumber);
    });
}

export function assertNoSuchTransactionOnConn(conn, lsid, txnNumber) {
    assert.commandFailedWithCode(
        conn.getDB("foo").runCommand({
            find: "bar",
            lsid: lsid,
            txnNumber: NumberLong(txnNumber),
            autocommit: false,
        }),
        ErrorCodes.NoSuchTransaction,
        "expected there to be no active transaction on shard, lsid: " + tojson(lsid) +
            ", txnNumber: " + tojson(txnNumber) + ", connection: " + tojson(conn));
}

export function waitForFailpoint(hitFailpointStr, numTimes, timeout) {
    // Don't run the hang analyzer because we don't expect waitForFailpoint() to always succeed.
    const hitFailpointRe = /Hit (\w+) failpoint/;
    const hitRe = /Hit (\w+)/;
    const matchHitFailpoint = hitFailpointStr.match(hitFailpointRe);
    const matchHit = hitFailpointStr.match(hitRe);
    if (matchHitFailpoint) {
        hitFailpointStr = `(Hit .+ failpoint.*${matchHitFailpoint[1]}|${hitFailpointStr})`;
    } else {
        hitFailpointStr = `(Hit .+.*${matchHit[1]}|${hitFailpointStr})`;
    }
    assert.soon(
        function() {
            const re = new RegExp(hitFailpointStr, 'g' /* find all occurrences */);
            return (rawMongoProgramOutput(hitFailpointStr).match(re) || []).length == numTimes;
        },
        'Failed to find "' + hitFailpointStr + '" logged ' + numTimes + ' times',
        timeout,
        undefined,
        {runHangAnalyzer: false});
}

/*
 * If all shards in the cluster have binVersion "lastest", sets the server parameter for
 * making the transaction coordinator return decision early to true.
 * TODO (SERVER-48114): Remove this function.
 */
export function enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(st) {
    st._rs.forEach(rs => {
        rs.nodes.forEach(node => {
            assert.commandWorked(node.getDB('admin').runCommand({
                setParameter: 1,
                "coordinateCommitReturnImmediatelyAfterPersistingDecision": true
            }));
        });
    });
}

// Enables the transaction router to retry on stale version (db or shard version) and snapshot
// errors within a transaction.
//
// TODO SERVER-39704: Remove this function.
export function enableStaleVersionAndSnapshotRetriesWithinTransactions(st) {
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "enableStaleVersionAndSnapshotRetriesWithinTransactions",
        mode: "alwaysOn"
    }));

    st._rs.forEach(function(replTest) {
        replTest.nodes.forEach(function(node) {
            assert.commandWorked(node.getDB('admin').runCommand(
                {configureFailPoint: "dontRemoveTxnCoordinatorOnAbort", mode: "alwaysOn"}));
        });
    });
}

// TODO SERVER-39704: Remove this function.
// Note: tests relying on this function should use kShardOptionsForDisabledStaleShardVersionRetries
// as part of their ShardingTest initialisation.
export function disableStaleVersionAndSnapshotRetriesWithinTransactions(st) {
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "enableStaleVersionAndSnapshotRetriesWithinTransactions",
        mode: "off"
    }));

    st._rs.forEach(function(replTest) {
        replTest.nodes.forEach(function(node) {
            assert.commandWorked(node.getDB('admin').runCommand(
                {configureFailPoint: "dontRemoveTxnCoordinatorOnAbort", mode: "off"}));
        });
    });
}

// Override value for the ShardingTest.other.rsOptions field to safely use
// disableStaleVersionAndSnapshotRetriesWithinTransactions().
export const kShardOptionsForDisabledStaleShardVersionRetries = {
    // Relax the default constraint for in-transaction metadata refreshes to avoid spurious
    // timeouts on low-performant test environments.
    // For ease of implementation, this reconfiguration is preferred  over the usage of
    // transaction retry loops, since:
    // - failpoints are used to alter the behaviour of the transaction machinery
    // - tests disabling retries are expected check for specific transaction error codes.
    setParameter: {metadataRefreshInTransactionMaxWaitMS: 5000}
};

// Flush each router's metadata and force refreshes on each shard for the given namespace and/or
// database names.
//
// TODO SERVER-39704: Remove this function.
export function flushRoutersAndRefreshShardMetadata(st, {ns, dbNames = []} = {}) {
    st._mongos.forEach((s) => {
        assert.commandWorked(s.adminCommand({flushRouterConfig: 1}));
    });

    st._rs.forEach((rs) => {
        if (ns) {
            assert.commandWorked(
                rs.test.getPrimary().adminCommand({_flushRoutingTableCacheUpdates: ns}));
        }

        dbNames.forEach((dbName) => {
            assert.commandWorked(
                rs.test.getPrimary().adminCommand({_flushDatabaseCacheUpdates: dbName}));
        });
    });
}

export function makeLsidFilter(lsid, fieldName) {
    const filter = {};
    for (let k of ["id", "txnUUID", "txnNumber"]) {
        if (k in lsid) {
            filter[fieldName + "." + k] = lsid[k];
        } else {
            filter[fieldName + "." + k] = {"$exists": false};
        }
    }
    return filter;
}

export function getOplogEntriesForTxnOnNode(node, lsid, txnNumber) {
    const filter = Object.assign(makeLsidFilter(lsid, "lsid"), {txnNumber: NumberLong(txnNumber)});
    return node.getCollection("local.oplog.rs").find(filter).sort({_id: 1}).toArray();
}

export function getOplogEntriesForTxn(rs, lsid, txnNumber) {
    return getOplogEntriesForTxnOnNode(rs.getPrimary(), lsid, txnNumber);
}

export function getTxnEntriesForSessionOnNode(node, lsid) {
    return node.getCollection("config.transactions")
        .find(makeLsidFilter(lsid, "_id"))
        .sort({_id: 1})
        .toArray();
}

export function getTxnEntriesForSession(rs, lsid) {
    return getTxnEntriesForSessionOnNode(rs.getPrimary(), lsid);
}

export function getImageEntriesForTxnOnNode(node, lsid, txnNumber) {
    const filter = Object.assign(makeLsidFilter(lsid, "_id"), {txnNum: NumberLong(txnNumber)});
    return node.getCollection("config.image_collection").find(filter).sort({_id: 1}).toArray();
}

export function getImageEntriesForTxn(rs, lsid, txnNumber) {
    return getImageEntriesForTxnOnNode(rs.getPrimary(), lsid, txnNumber);
}

export function makeAbortTransactionCmdObj(lsid, txnNumber) {
    return {
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    };
}

export function makeCommitTransactionCmdObj(lsid, txnNumber) {
    return {
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    };
}

export function makePrepareTransactionCmdObj(lsid, txnNumber) {
    return {
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    };
}

export function isUpdateDocumentShardKeyUsingTransactionApiEnabled(conn) {
    return jsTestOptions().mongosBinVersion !== "last-lts" &&
        jsTestOptions().mongosBinVersion !== "last-continuous" &&
        FeatureFlagUtil.isEnabled(conn, "UpdateDocumentShardKeyUsingTransactionApi");
}
