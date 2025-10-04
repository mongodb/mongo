/**
 * Helper functions for testing prepared transactions.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

export const PrepareHelpers = (function () {
    /**
     * Prepares the active transaction on the session. This expects the 'prepareTransaction' command
     * to succeed and return a non-null 'prepareTimestamp'.
     *
     * @return {Timestamp} the transaction's prepareTimestamp
     */
    function prepareTransaction(session, writeConcernOption = {w: "majority"}) {
        assert(session);

        const res = assert.commandWorked(
            session.getDatabase("admin").adminCommand({prepareTransaction: 1, writeConcern: writeConcernOption}),
        );
        assert(res.prepareTimestamp, "prepareTransaction did not return a 'prepareTimestamp': " + tojson(res));
        const prepareTimestamp = res.prepareTimestamp;
        assert(prepareTimestamp instanceof Timestamp, "prepareTimestamp was not a Timestamp: " + tojson(res));
        assert.neq(prepareTimestamp, Timestamp(0, 0), "prepareTimestamp cannot be null: " + tojson(res));
        return prepareTimestamp;
    }

    /**
     * Commits the active transaction on the session.
     *
     * @return {object} the response to the 'commitTransaction' command.
     */
    function commitTransaction(session, commitTimestamp) {
        assert(session);

        let cmd = {commitTransaction: 1, commitTimestamp: commitTimestamp};
        const writeConcern = session.getTxnWriteConcern_forTesting();
        if (writeConcern !== undefined) {
            cmd.writeConcern = writeConcern;
        }

        const res = session.getDatabase("admin").adminCommand(cmd);

        // End the transaction on the shell session.
        if (res.ok) {
            assert.commandWorked(session.commitTransaction_forTesting());
        } else {
            assert.commandWorkedOrFailedWithCode(session.abortTransaction_forTesting(), ErrorCodes.NoSuchTransaction);
        }
        return res;
    }

    /**
     * Creates a session object on the given connection with the provided 'lsid'.
     *
     * @return {session} the session created.
     */
    function createSessionWithGivenId(conn, lsid, sessionOptions = {}) {
        const session = conn.startSession(sessionOptions);

        const oldId = session._serverSession.handle.getId();
        print("Overriding sessionID " + tojson(oldId) + " with " + tojson(lsid) + " for test.");
        session._serverSession.handle.getId = () => lsid;

        return session;
    }

    const oplogSizeMB = 1;
    const oplogSizeBytes = oplogSizeMB * 1024 * 1024;
    const tenKB = "a".repeat(10 * 1024);

    /**
     * Writes until the oplog exceeds its configured maximum, proving that the node keeps as much
     * oplog as necessary to preserve entries for the oldest active transaction.
     */
    function growOplogPastMaxSize(replSet) {
        const primary = replSet.getPrimary();
        const oplog = primary.getDB("local").oplog.rs;
        assert.lte(oplog.dataSize(), oplogSizeBytes);
        const coll = primary.getDB("growOplogPastMaxSize").growOplogPastMaxSize;
        const numNodes = replSet.nodeList().length;
        while (oplog.dataSize() <= 2 * oplogSizeBytes) {
            assert.commandWorked(coll.insert({tenKB: tenKB}, {writeConcern: {w: numNodes}}));
        }

        print(`Oplog on ${primary} dataSize = ${oplog.dataSize()}`);
    }

    /**
     * Waits for the oplog to be truncated, proving that once a transaction finishes its oplog
     * entries can be reclaimed.
     */
    function awaitOplogTruncation(replSet) {
        print(`Waiting for oplog to shrink to ${oplogSizeMB} MB`);
        const primary = replSet.getPrimary();
        const primaryOplog = primary.getDB("local").oplog.rs;
        const secondary = replSet.getSecondary();
        const secondaryOplog = secondary.getDB("local").oplog.rs;

        // Old entries are reclaimed when oplog size reaches new milestone. With a 1MB oplog,
        // milestones are every 0.1 MB (see WiredTigerRecordStore::OplogTruncateMarkers) so write
        // about 0.2 MB to be certain.
        print("Add writes after transaction finished to trigger oplog reclamation");
        const tenKB = "a".repeat(10 * 1024);
        const coll = primary.getDB("awaitOplogTruncation").awaitOplogTruncation;
        const numNodes = replSet.nodeList().length;
        for (let i = 0; i < 20; i++) {
            assert.commandWorked(coll.insert({tenKB: tenKB}, {writeConcern: {w: numNodes}}));
        }

        for (let [nodeName, oplog] of [
            ["primary", primaryOplog],
            ["secondary", secondaryOplog],
        ]) {
            assert.soon(
                function () {
                    const dataSize = oplog.dataSize();
                    const prepareEntryRemoved = oplog.findOne({prepare: true}) === null;
                    print(
                        `${nodeName} oplog dataSize: ${dataSize},` + ` prepare entry removed: ${prepareEntryRemoved}`,
                    );

                    // The oplog milestone system allows the oplog to grow to 110% its max size.
                    if (dataSize < 1.1 * oplogSizeBytes && prepareEntryRemoved) {
                        return true;
                    }

                    assert.commandWorked(coll.insert({tenKB: tenKB}, {writeConcern: {w: numNodes}}));
                    return false;
                },
                `waiting for ${nodeName} oplog reclamation`,
                ReplSetTest.kDefaultTimeoutMS,
                1000,
            );
        }
    }

    /**
     * Waits for the oplog entry of the given timestamp to be majority committed.
     */
    function awaitMajorityCommitted(replSet, timestamp) {
        print(`Waiting for majority commit point to advance past the given timestamp ${tojson(timestamp)}`);
        const primary = replSet.getPrimary();
        assert.soon(
            () => {
                const ts = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1})).optimes.lastCommittedOpTime
                    .ts;
                if (timestampCmp(ts, timestamp) >= 0) {
                    print(`Finished awaiting lastCommittedOpTime.ts, now at ${tojson(ts)}`);
                    return true;
                } else {
                    print(`Awaiting lastCommittedOpTime.ts, now at ${tojson(ts)}`);
                    return false;
                }
            },
            "Timeout waiting for majority commit point",
            ReplSetTest.kDefaultTimeoutMS,
            1000,
        );
    }

    function findPrepareEntry(oplogColl) {
        return oplogColl.findOne({op: "c", "o.prepare": true});
    }

    return {
        prepareTransaction: prepareTransaction,
        commitTransaction: commitTransaction,
        createSessionWithGivenId: createSessionWithGivenId,
        oplogSizeMB: oplogSizeMB,
        oplogSizeBytes: oplogSizeBytes,
        replSetStartSetOptions: {oplogSize: oplogSizeMB},
        growOplogPastMaxSize: growOplogPastMaxSize,
        awaitOplogTruncation: awaitOplogTruncation,
        awaitMajorityCommitted: awaitMajorityCommitted,
        findPrepareEntry: findPrepareEntry,
    };
})();
