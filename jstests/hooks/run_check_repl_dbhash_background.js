/**
 * Runs the dbHash command across all members of a replica set and compares the output.
 *
 * Unlike run_check_repl_dbhash.js, this version of the hook doesn't require that all operations
 * have finished replicating, nor does it require that the test has finished running. The dbHash
 * command is run inside of a transaction specifying atClusterTime in order for an identical
 * snapshot to be used by all members of the replica set.
 *
 * The find and getMore commands used to generate the collection diff run as part of the same
 * transaction as the dbHash command. This ensures the diagnostics for a dbhash mismatch aren't
 * subjected to changes from any operations in flight.
 *
 * If a transient transaction error occurs, then the dbhash check is retried until it succeeds, or
 * until it fails with a non-transient error.
 */
'use strict';

(function() {
    load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.

    if (typeof db === 'undefined') {
        throw new Error(
            "Expected mongo shell to be connected a server, but global 'db' object isn't defined");
    }

    // We turn off printing the JavaScript stacktrace in doassert() to avoid generating an
    // overwhelming amount of log messages when handling transient transaction errors.
    TestData = TestData || {};
    TestData.traceExceptions = false;

    // Calls 'func' with the print() function overridden to be a no-op.
    const quietly = (func) => {
        const printOriginal = print;
        try {
            print = Function.prototype;
            func();
        } finally {
            print = printOriginal;
        }
    };

    const conn = db.getMongo();
    const topology = DiscoverTopology.findConnectedNodes(conn);

    if (topology.type !== Topology.kReplicaSet) {
        throw new Error('Unsupported topology configuration: ' + tojson(topology));
    }

    let rst;

    // We construct the ReplSetTest instance with the print() function overridden to be a no-op in
    // order to suppress the log messages about the replica set configuration. The
    // run_check_repl_dbhash_background.js hook is executed frequently by resmoke.py and would
    // otherwise lead to generating an overwhelming amount of log messages.
    quietly(() => {
        rst = new ReplSetTest(topology.nodes[0]);
    });

    const sessions = [
        rst.getPrimary(),
        ...rst.getSecondaries().filter(conn => {
            return !conn.adminCommand({isMaster: 1}).arbiterOnly;
        })
    ].map(conn => conn.startSession({causalConsistency: false}));

    const resetFns = [];
    const kForeverSeconds = 1e9;
    const dbNames = new Set();

    // We enable the "WTPreserveSnapshotHistoryIndefinitely" failpoint and extend the
    // "transactionLifetimeLimitSeconds" server parameter to ensure that
    //
    //   (1) the same snapshot will be available to read at on the primary and secondaries, and
    //
    //   (2) the potentally long-running transaction isn't killed while we are part-way through
    //       verifying data consistency.
    for (let session of sessions) {
        const db = session.getDatabase('admin');

        assert.commandWorked(db.runCommand({
            configureFailPoint: 'WTPreserveSnapshotHistoryIndefinitely',
            mode: 'alwaysOn',
        }));

        resetFns.push(() => {
            assert.commandWorked(db.runCommand({
                configureFailPoint: 'WTPreserveSnapshotHistoryIndefinitely',
                mode: 'off',
            }));
        });

        const res = assert.commandWorked(db.runCommand({
            setParameter: 1,
            transactionLifetimeLimitSeconds: kForeverSeconds,
        }));

        const transactionLifetimeLimitSecondsOriginal = res.was;
        resetFns.push(() => {
            assert.commandWorked(db.runCommand({
                setParameter: 1,
                transactionLifetimeLimitSeconds: transactionLifetimeLimitSecondsOriginal,
            }));
        });
    }

    // We run the "listDatabases" command on each of the nodes after having enabled the
    // "WTPreserveSnapshotHistoryIndefinitely" failpoint on all of the nodes so that
    // 'sessions[0].getOperationTime()' is guaranteed to be greater than the clusterTime at which
    // the secondaries have stopped truncating their snapshot history.
    for (let session of sessions) {
        const db = session.getDatabase('admin');
        const res = assert.commandWorked(db.runCommand({listDatabases: 1, nameOnly: true}));
        for (let dbInfo of res.databases) {
            dbNames.add(dbInfo.name);
        }
    }

    // Transactions cannot be run on the following databases. (The "local" database is also not
    // replicated.)
    dbNames.delete('admin');
    dbNames.delete('config');
    dbNames.delete('local');

    const results = [];

    // The waitForSecondaries() function waits for all secondaries to have applied up to
    // 'clusterTime' locally. This ensures that a later atClusterTime read inside a transaction
    // doesn't stall as a result of a pending global X lock (e.g. from a dropDatabase command) on
    // the primary preventing getMores on the oplog from receiving a response.
    const waitForSecondaries = (clusterTime) => {
        for (let i = 1; i < sessions.length; ++i) {
            const session = sessions[i];
            const db = session.getDatabase('admin');

            // We advance the clusterTime on the secondary's session to ensure that 'clusterTime'
            // doesn't exceed the node's notion of the latest clusterTime.
            session.advanceClusterTime(sessions[0].getClusterTime());

            // We do an afterClusterTime read on a nonexistent collection to wait for the secondary
            // to have applied up to 'clusterTime' and advanced its majority commit point.
            assert.commandWorked(db.runCommand({
                find: 'run_check_repl_dbhash_background',
                readConcern: {level: 'majority', afterClusterTime: clusterTime},
                limit: 1,
                singleBatch: true,
            }));
        }
    };

    // The checkCollectionHashesForDB() function identifies a collection by its UUID and ignores the
    // case where a collection isn't present on a node to work around how the collection catalog
    // isn't multi-versioned. Unlike with ReplSetTest#checkReplicatedDataHashes(), it is possible
    // for a collection catalog operation (e.g. a drop or rename) to have been applied on the
    // primary but not yet applied on the secondary.
    const checkCollectionHashesForDB = (dbName) => {
        const result = [];
        const hashes = rst.getHashesUsingSessions(sessions, dbName);
        const hashesByUUID = hashes.map((response, i) => {
            const info = {};

            for (let collName of Object.keys(response.collections)) {
                const hash = response.collections[collName];
                const uuid = response.uuids[collName];
                if (uuid !== undefined) {
                    info[uuid.toString()] = {
                        host: sessions[i].getClient().host,
                        hash,
                        collName,
                        uuid,
                    };
                }
            }

            return Object.assign({}, response, {hashesByUUID: info});
        });

        const primarySession = sessions[0];
        for (let i = 1; i < hashes.length; ++i) {
            const uuids = new Set([
                ...Object.keys(hashesByUUID[0].hashesByUUID),
                ...Object.keys(hashesByUUID[i].hashesByUUID),
            ]);

            const secondarySession = sessions[i];
            for (let uuid of uuids) {
                const primaryInfo = hashesByUUID[0].hashesByUUID[uuid];
                const secondaryInfo = hashesByUUID[i].hashesByUUID[uuid];

                if (primaryInfo === undefined) {
                    print("Skipping collection because it doesn't exist on the primary: " +
                          tojsononeline(secondaryInfo));
                    continue;
                }

                if (secondaryInfo === undefined) {
                    print("Skipping collection because it doesn't exist on the secondary: " +
                          tojsononeline(primaryInfo));
                    continue;
                }

                if (primaryInfo.hash !== secondaryInfo.hash) {
                    const diff = rst.getCollectionDiffUsingSessions(
                        primarySession, secondarySession, dbName, primaryInfo.uuid);

                    result.push({
                        primary: primaryInfo,
                        secondary: secondaryInfo,
                        dbName: dbName,
                        diff: diff,
                    });
                }
            }
        }

        return result;
    };

    for (let dbName of dbNames) {
        let hasTransientError;

        do {
            const clusterTime = sessions[0].getOperationTime();
            waitForSecondaries(clusterTime);

            for (let session of sessions) {
                session.startTransaction(
                    {readConcern: {level: 'snapshot', atClusterTime: clusterTime}});
            }

            hasTransientError = false;

            try {
                const result = checkCollectionHashesForDB(dbName);

                for (let session of sessions) {
                    session.commitTransaction();
                }

                for (let mismatchInfo of result) {
                    mismatchInfo.atClusterTime = clusterTime;
                    results.push(mismatchInfo);
                }
            } catch (e) {
                for (let session of sessions) {
                    session.abortTransaction();
                }

                if (e.hasOwnProperty('errorLabels') &&
                    e.errorLabels.includes('TransientTransactionError')) {
                    hasTransientError = true;
                    continue;
                }

                throw e;
            }
        } while (hasTransientError);
    }

    for (let resetFn of resetFns) {
        resetFn();
    }

    const headings = [];
    let errorBlob = '';

    for (let mismatchInfo of results) {
        const diff = mismatchInfo.diff;
        delete mismatchInfo.diff;

        const heading =
            `dbhash mismatch for ${mismatchInfo.dbName}.${mismatchInfo.primary.collName}`;

        headings.push(heading);

        if (headings.length > 1) {
            errorBlob += '\n\n';
        }
        errorBlob += heading;
        errorBlob += `: ${tojson(mismatchInfo)}`;

        if (diff.docsWithDifferentContents.length > 0) {
            errorBlob += '\nThe following documents have different contents on the primary and' +
                ' secondary:';
            for (let {
                     primary, secondary
                 } of diff.docsWithDifferentContents) {
                errorBlob += `\n  primary:   ${tojsononeline(primary)}`;
                errorBlob += `\n  secondary: ${tojsononeline(secondary)}`;
            }
        } else {
            errorBlob += '\nNo documents have different contents on the primary and secondary';
        }

        if (diff.docsMissingOnPrimary.length > 0) {
            errorBlob += "\nThe following documents aren't present on the primary:";
            for (let doc of diff.docsMissingOnPrimary) {
                errorBlob += `\n  ${tojsononeline(doc)}`;
            }
        } else {
            errorBlob += '\nNo documents are missing from the primary';
        }

        if (diff.docsMissingOnSecondary.length > 0) {
            errorBlob += "\nThe following documents aren't present on the secondary:";
            for (let doc of diff.docsMissingOnSecondary) {
                errorBlob += `\n  ${tojsononeline(doc)}`;
            }
        } else {
            errorBlob += '\nNo documents are missing from the secondary';
        }
    }

    if (headings.length > 0) {
        for (let session of sessions) {
            const query = {};
            const limit = 100;
            rst.dumpOplog(session.getClient(), query, limit);
        }

        print(errorBlob);
        throw new Error(`dbhash mismatch (search for the following headings): ${tojson(headings)}`);
    }
})();
