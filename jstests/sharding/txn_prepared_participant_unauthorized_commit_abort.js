/**
 * Tests that a user cannot commit or abort a prepared transaction on an individual txn participant (shard).
 *
 * @tags: [
 *   uses_transactions,
 *   uses_prepare_transaction,
 *   uses_multi_shard_transaction,
 *   requires_sharding,
 *   requires_persistence,
 *   # The test freezes the coordinator at a failpoint and drives a precise 2PC race; config-server
 *   # stepdowns would disrupt the coordinator and the frozen participant state.
 *   does_not_support_stepdowns,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {getOplogEntriesForTxnOnNode} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const kKeyFile = "jstests/libs/key1";
const kDbName = "testdb";
const kCollName = "sharded_coll";
const kNs = kDbName + "." + kCollName;
// User with only readWrite on their own database -- no directShardOperations, no cluster/internal
// privileges.
const kUser = "rwuser";
const kUserPwd = "rwuserPwd";
// The failpoint that freezes the coordinator after all participants have prepared and voted, but
// before any commit/abort decision is written or sent.
const kCoordinatorFp = "hangBeforeWritingDecision";

// Authenticates as the user and commits the cross-shard transaction through mongos.
// This drives the 2PC coordinator to prepare all participants and then blocks at the coordinator
// failpoint until the main thread releases it.
// Returns the raw command response so the caller can inspect it.
function commitThroughMongosAsUser(mongosHost, lsidUUIDStr, txnNumber, authDb, user, pwd) {
    const conn = new Mongo(mongosHost);
    assert(conn.getDB(authDb).auth(user, pwd), "user failed to authenticate");
    return conn.getDB("admin").runCommand({
        commitTransaction: 1,
        lsid: {id: UUID(lsidUUIDStr)},
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
    });
}

// Reading the oplog requires privileges on the 'local' database, so do it as the cluster.
function getCommitOplogEntryOnNode(conn, lsid, txnNumber) {
    return authutil.asCluster(conn, kKeyFile, () => {
        const entries = getOplogEntriesForTxnOnNode(conn, lsid, txnNumber);
        return entries.find((e) => e.o?.commitTransaction !== undefined);
    });
}

describe("Unauthorized commit/abort of a prepared cross-shard 2PC participant", function () {
    before(() => {
        this.st = new ShardingTest({
            name: jsTestName(),
            shards: 2,
            rs: {nodes: 1},
            keyFile: kKeyFile,
        });

        // rs0's primary is the 2PC coordinator (every transaction below writes to shard0 first).
        this.coordPrimary = this.st.rs0.getPrimary();
        // rs1's primary is a pure participant -- the shard the user will try to commit/abort on directly.
        this.participantPrimary = this.st.rs1.getPrimary();

        // Create the low-privilege user and set up sharding as the cluster (__system).
        const userRoles = [{role: "readWrite", db: kDbName}];
        authutil.asCluster(this.st.s, kKeyFile, () => {
            // The user authenticates against mongos (config-server user) to run the transaction.
            this.st.s.getDB(kDbName).createUser({user: kUser, pwd: kUserPwd, roles: userRoles});

            // Shard the collection so _id < 0 lives on shard0 and _id >= 0 lives on shard1.
            assert.commandWorked(
                this.st.s.adminCommand({
                    enableSharding: kDbName,
                    primaryShard: this.st.shard0.shardName,
                }),
            );
            assert.commandWorked(this.st.s.adminCommand({shardCollection: kNs, key: {_id: 1}}));
            assert.commandWorked(this.st.s.adminCommand({split: kNs, middle: {_id: 0}}));
            assert.commandWorked(
                this.st.s.adminCommand({
                    moveChunk: kNs,
                    find: {_id: 0},
                    to: this.st.shard1.shardName,
                }),
            );
        });

        // A direct connection to a shard resolves users from that shard's *local* store, so create
        // the same user there too. The logical-session uid is SHA256("rwuser@<db>"), i.e.
        // purely name-based, so this shard-local user yields the identical uid the coordinator used
        // when it prepared the participant -- the user's direct commit/abort targets that exact
        // prepared session.
        authutil.asCluster(this.participantPrimary, kKeyFile, () => {
            this.participantPrimary.getDB(kDbName).createUser({
                user: kUser,
                pwd: kUserPwd,
                roles: userRoles,
            });
        });
    });

    after(() => {
        if (this.st) {
            this.st.stop();
        }
    });

    // Starts a cross-shard transaction as the user via mongos. The first statement targets
    // shard0 (making shard0 the coordinator); the second targets shard1.
    const startCrossShardTxn = (lsid, txnNumber, idOnShard0, idOnShard1) => {
        const user = new Mongo(this.st.s.host);
        assert(user.getDB(kDbName).auth(kUser, kUserPwd));

        assert.commandWorked(
            user.getDB(kDbName).runCommand({
                insert: kCollName,
                documents: [{_id: idOnShard0}],
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(0),
                startTransaction: true,
                autocommit: false,
            }),
        );
        assert.commandWorked(
            user.getDB(kDbName).runCommand({
                insert: kCollName,
                documents: [{_id: idOnShard1}],
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                stmtId: NumberInt(1),
                autocommit: false,
            }),
        );
    };

    // Kicks off the client commit through mongos on a background thread and waits for the
    // coordinator to reach the failpoint -- at which point every participant is prepared.
    const prepareAllParticipantsAndFreezeCoordinator = (lsid, txnNumber) => {
        // Enable the coordinator failpoint as the cluster (__system). configureFailPoint returns a
        // handle whose wait()/off() also issue commands, so they must likewise run while
        // authenticated as the cluster (asCluster logs the connection out when it returns).
        const fp = authutil.asCluster(this.coordPrimary, kKeyFile, () =>
            configureFailPoint(this.coordPrimary, kCoordinatorFp),
        );

        const commitThread = new Thread(
            commitThroughMongosAsUser,
            this.st.s.host,
            extractUUIDFromObject(lsid.id),
            txnNumber,
            kDbName,
            kUser,
            kUserPwd,
        );
        commitThread.start();

        // Wait until the coordinator hits the failpoint -- at which point every participant is
        // prepared.
        authutil.asCluster(this.coordPrimary, kKeyFile, () => fp.wait());
        return {commitThread, fp};
    };

    it("rejects a non-coordinator client committing a prepared participant, preserving atomicity", () => {
        const lsid = {id: UUID()};
        const txnNumber = 4242;
        const idOnShard0 = -1;
        const idOnShard1 = 1;

        startCrossShardTxn(lsid, txnNumber, idOnShard0, idOnShard1);
        const {commitThread, fp} = prepareAllParticipantsAndFreezeCoordinator(lsid, txnNumber);

        try {
            // The user opens a DIRECT connection to the pure-participant shard and tries to commit
            // their own prepared participant at a future timestamp -- no coordinator.
            const userDirect = new Mongo(this.participantPrimary.host);
            assert(userDirect.getDB(kDbName).auth(kUser, kUserPwd));

            const nowTs = userDirect.adminCommand({hello: 1}).$clusterTime.clusterTime;
            const wrongCommitTs = Timestamp(nowTs.getTime() + 60, 0);

            const commitRes = userDirect.getDB("admin").runCommand({
                commitTransaction: 1,
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                autocommit: false,
                commitTimestamp: wrongCommitTs,
                writeConcern: {w: "majority"},
            });
            // A non-coordinator client must not be able to commit a prepared 2PC participant.
            assert.commandFailedWithCode(
                commitRes,
                ErrorCodes.Unauthorized,
                "A non-coordinator client's direct commit of a prepared participant should be rejected",
            );
        } finally {
            // Always release the coordinator and join the client commit so the cluster is left
            // clean whether or not the command was (correctly) rejected.
            authutil.asCluster(this.coordPrimary, kKeyFile, () => fp.off());
            commitThread.join();
        }

        assert.commandWorked(commitThread.returnData(), "the client's commit through mongos should succeed");

        // Atomicity holds: the coordinator drove both shards to commit at its single decision
        // timestamp, and both writes are durable.
        const coordCommit = getCommitOplogEntryOnNode(this.coordPrimary, lsid, txnNumber);
        const partCommit = getCommitOplogEntryOnNode(this.participantPrimary, lsid, txnNumber);
        assert(coordCommit, "coordinator shard is missing a commit oplog entry");
        assert(partCommit, "participant shard is missing a commit oplog entry");
        assert.eq(
            0,
            timestampCmp(coordCommit.o.commitTimestamp, partCommit.o.commitTimestamp),
            "both shards must commit at the coordinator's single decision timestamp",
            {
                coordinatorCommitTs: coordCommit.o.commitTimestamp,
                participantCommitTs: partCommit.o.commitTimestamp,
            },
        );

        const user = new Mongo(this.st.s.host);
        assert(user.getDB(kDbName).auth(kUser, kUserPwd));
        assert.eq(
            2,
            user
                .getDB(kDbName)
                .getCollection(kCollName)
                .find({_id: {$in: [idOnShard0, idOnShard1]}})
                .itcount(),
            "both of the transaction's writes should be committed and visible",
        );
    });

    it("rejects a non-coordinator client aborting a prepared participant, preserving atomicity", () => {
        const lsid = {id: UUID()};
        const txnNumber = 4343;
        const idOnShard0 = -2;
        const idOnShard1 = 2;

        startCrossShardTxn(lsid, txnNumber, idOnShard0, idOnShard1);
        const {commitThread, fp} = prepareAllParticipantsAndFreezeCoordinator(lsid, txnNumber);

        try {
            // The user opens a DIRECT connection to the pure-participant shard and tries to abort
            // their own prepared participant, while every peer is set to commit.
            const userDirect = new Mongo(this.participantPrimary.host);
            assert(userDirect.getDB(kDbName).auth(kUser, kUserPwd));

            const abortRes = userDirect.getDB("admin").runCommand({
                abortTransaction: 1,
                lsid: lsid,
                txnNumber: NumberLong(txnNumber),
                autocommit: false,
            });
            // A non-coordinator client must not be able to abort a prepared 2PC participant.
            assert.commandFailedWithCode(
                abortRes,
                ErrorCodes.Unauthorized,
                "A non-coordinator client's direct abort of a prepared participant should be rejected",
            );
        } finally {
            authutil.asCluster(this.coordPrimary, kKeyFile, () => fp.off());
            commitThread.join();
        }

        assert.commandWorked(commitThread.returnData(), "the client's commit through mongos should succeed");

        // Atomicity holds: the whole transaction committed; no write was lost to a unilateral abort.
        const user = new Mongo(this.st.s.host);
        assert(user.getDB(kDbName).auth(kUser, kUserPwd));
        assert.eq(
            2,
            user
                .getDB(kDbName)
                .getCollection(kCollName)
                .find({_id: {$in: [idOnShard0, idOnShard1]}})
                .itcount(),
            "both of the transaction's writes should be committed and visible",
        );
        assert(
            getCommitOplogEntryOnNode(this.participantPrimary, lsid, txnNumber),
            "participant shard should have committed, not aborted",
        );
    });
});
