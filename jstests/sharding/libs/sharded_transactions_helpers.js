load("jstests/libs/logv2_helpers.js");

const kSnapshotErrors =
    [ErrorCodes.SnapshotTooOld, ErrorCodes.SnapshotUnavailable, ErrorCodes.StaleChunkHistory];

// List of failpoints in the coordinator's two-phase commit code. The associated data describes how
// many times each failpoint would be hit assuming a 3-participant transaction where one of the
// participants is co-located with the coordinator:
// - numTimesShouldBeHit: N means the failpoint should be hit N times during that phase; for
//   example, if there are two remote participants, the hangWhileTargetingRemoteHost failpoint would
//   be hit two times in the prepare phase.
// - skip: N means turn on the failpoint after the failpoint has been hit N times; it's used to turn
//   on the remote and local targeting failpoints for the prepare and decision phase separately.
function getCoordinatorFailpoints() {
    const coordinatorFailpointDataArr = [
        {failpoint: "hangBeforeWritingParticipantList", numTimesShouldBeHit: 1},
        {
            // Test targeting remote nodes for prepare
            failpoint: "hangWhileTargetingRemoteHost",
            numTimesShouldBeHit: 2 /* once per remote participant */
        },
        {
            // Test targeting local node for prepare
            failpoint: "hangWhileTargetingLocalHost",
            numTimesShouldBeHit: 1
        },
        {failpoint: "hangBeforeWritingDecision", numTimesShouldBeHit: 1},
        {
            // Test targeting remote nodes for decision
            failpoint: "hangWhileTargetingRemoteHost",
            numTimesShouldBeHit: 2, /* once per remote participant */
            skip: 2                 /* to skip when the failpoint is hit for prepare */
        },
        {
            // Test targeting local node for decision
            failpoint: "hangWhileTargetingLocalHost",
            numTimesShouldBeHit: 1,
            skip: 1 /* to skip when the failpoint is hit for prepare */
        },
        {failpoint: "hangBeforeDeletingCoordinatorDoc", numTimesShouldBeHit: 1},
    ];

    // Return a deep copy of the array, so that the caller is free to modify its contents.
    return coordinatorFailpointDataArr.map(failpoint => Object.assign({}, failpoint));
}

function setFailCommandOnShards(st, mode, commands, code, numShards, ns) {
    for (let i = 0; i < numShards; i++) {
        const shardConn = st["rs" + i].getPrimary();
        // Sharding tests require failInternalCommands: true, since the mongos appears to mongod to
        // be an internal client.
        if (ns) {
            assert.commandWorked(shardConn.adminCommand({
                configureFailPoint: "failCommand",
                mode: mode,
                data: {
                    namespace: ns,
                    errorCode: code,
                    failCommands: commands,
                    failInternalCommands: true
                }
            }));
        } else {
            assert.commandWorked(shardConn.adminCommand({
                configureFailPoint: "failCommand",
                mode: mode,
                data: {errorCode: code, failCommands: commands, failInternalCommands: true}
            }));
        }
    }
}

function unsetFailCommandOnEachShard(st, numShards) {
    for (let i = 0; i < numShards; i++) {
        const shardConn = st["rs" + i].getPrimary();
        assert.commandWorked(
            shardConn.adminCommand({configureFailPoint: "failCommand", mode: "off"}));
    }
}

function assertNoSuchTransactionOnAllShards(st, lsid, txnNumber) {
    st._rs.forEach(function(rs) {
        assertNoSuchTransactionOnConn(rs.test.getPrimary(), lsid, txnNumber);
    });
}

function assertNoSuchTransactionOnConn(conn, lsid, txnNumber) {
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

function waitForFailpoint(hitFailpointStr, numTimes, timeout) {
    // Don't run the hang analyzer because we don't expect waitForFailpoint() to always succeed.
    if (isJsonLogNoConn()) {
        const hitFailpointRe = /Hit (\w+) failpoint/;
        const hitRe = /Hit (\w+)/;
        const matchHitFailpoint = hitFailpointStr.match(hitFailpointRe);
        const matchHit = hitFailpointStr.match(hitRe);
        if (matchHitFailpoint) {
            hitFailpointStr = `(Hit .+ failpoint.*${matchHitFailpoint[1]}|${hitFailpointStr})`;
        } else {
            hitFailpointStr = `(Hit .+.*${matchHit[1]}|${hitFailpointStr})`;
        }
    }
    assert.soon(
        function() {
            const re = new RegExp(hitFailpointStr, 'g' /* find all occurrences */);
            return (rawMongoProgramOutput().match(re) || []).length == numTimes;
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
function enableCoordinateCommitReturnImmediatelyAfterPersistingDecision(st) {
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
function enableStaleVersionAndSnapshotRetriesWithinTransactions(st) {
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
function disableStaleVersionAndSnapshotRetriesWithinTransactions(st) {
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

// Flush each router's metadata and force refreshes on each shard for the given namespace and/or
// database names.
//
// TODO SERVER-39704: Remove this function.
function flushRoutersAndRefreshShardMetadata(st, {ns, dbNames = []} = {}) {
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

function makeLsidFilter(lsid, fieldName) {
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

function getOplogEntriesForTxnOnNode(node, lsid, txnNumber) {
    const filter = Object.assign(makeLsidFilter(lsid, "lsid"), {txnNumber: NumberLong(txnNumber)});
    return node.getCollection("local.oplog.rs").find(filter).sort({_id: 1}).toArray();
}

function getOplogEntriesForTxn(rs, lsid, txnNumber) {
    return getOplogEntriesForTxnOnNode(rs.getPrimary(), lsid, txnNumber);
}

function getTxnEntriesForSessionOnNode(node, lsid) {
    return node.getCollection("config.transactions")
        .find(makeLsidFilter(lsid, "_id"))
        .sort({_id: 1})
        .toArray();
}

function getTxnEntriesForSession(rs, lsid) {
    return getTxnEntriesForSessionOnNode(rs.getPrimary(), lsid);
}

function getImageEntriesForTxnOnNode(node, lsid, txnNumber) {
    const filter = Object.assign(makeLsidFilter(lsid, "_id"), {txnNum: NumberLong(txnNumber)});
    return node.getCollection("config.image_collection").find(filter).sort({_id: 1}).toArray();
}

function getImageEntriesForTxn(rs, lsid, txnNumber) {
    return getImageEntriesForTxnOnNode(rs.getPrimary(), lsid, txnNumber);
}

function makeAbortTransactionCmdObj(lsid, txnNumber) {
    return {
        abortTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    };
}

function makeCommitTransactionCmdObj(lsid, txnNumber) {
    return {
        commitTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    };
}

function makePrepareTransactionCmdObj(lsid, txnNumber) {
    return {
        prepareTransaction: 1,
        lsid: lsid,
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: "majority"},
    };
}

function isUpdateDocumentShardKeyUsingTransactionApiEnabled(conn) {
    return jsTestOptions().mongosBinVersion !== "last-lts" &&
        jsTestOptions().mongosBinVersion !== "last-continuous" &&
        assert
            .commandWorked(conn.adminCommand(
                {getParameter: 1, featureFlagUpdateDocumentShardKeyUsingTransactionApi: 1}))
            .featureFlagUpdateDocumentShardKeyUsingTransactionApi.value;
}
