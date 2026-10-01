/**
 * Verifies that a user holding only the 'fsync' privilege cannot release the cluster fsync lock
 * held by another user on a sharded cluster.
 *
 * @tags: [
 *   requires_fsync,
 * ]
 */
import {before, after, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kKeyFile = "jstests/libs/key1";
const kPassword = "password";

// Returns [nodeName, nodeResponse] pairs from a cluster-wide fsync or fsyncUnlock response.
function getNodeResponses(res, containerField) {
    const container = containerField ? res[containerField] : res;
    assert(container.hasOwnProperty("raw"), "response missing raw section", {res});
    return Object.entries(container.raw);
}

function assertLockCount(res, containerField, expectedLockCount, msg) {
    for (const [node, nodeRes] of getNodeResponses(res, containerField)) {
        assert.eq(nodeRes.lockCount, expectedLockCount, msg + " (node: " + node + ")", {nodeRes});
    }
}

describe("Cluster fsync lock authorization on mongos", function () {
    const st = new ShardingTest({shards: 2, mongos: 1, config: 1, keyFile: kKeyFile});

    after(function () {
        // If a test case failed while locks were held, teardown would block behind the fsync
        // lock, so release every held count before stopping the cluster.
        if (!adminDb) {
            st.stop();
            return;
        }
        try {
            // A cluster fsyncUnlock reports ok:0 as soon as any target is already unlocked, but its
            // raw section still shows the per-target lockCount. Keep unlocking until no target
            // reports a held count, and bound the loop in case a target keeps failing.
            for (let i = 0; i < 50; ++i) {
                const res = adminDb.runCommand({fsyncUnlock: 1});
                const anyNodeLocked = Object.values(res.raw ?? {}).some(
                    (nodeRes) => nodeRes.ok && nodeRes.lockCount > 0,
                );
                if (!anyNodeLocked) {
                    break;
                }
            }
        } catch (e) {
            // Best-effort cleanup; ignore errors.
        }
        st.stop();
    });

    let adminDb;
    let fsyncUserDb;

    before(function () {
        adminDb = st.s.getDB("admin");

        // The first user closes the localhost exception, so re-authenticate afterwards.
        assert.commandWorked(
            adminDb.runCommand({createUser: "root", pwd: kPassword, roles: ["root"]}),
        );
        assert(adminDb.logout());
        assert(adminDb.auth("root", kPassword));

        // The fsync-only user can take the lock but cannot release it.
        assert.commandWorked(
            adminDb.runCommand({
                createRole: "fsyncOnlyRole",
                roles: [],
                privileges: [{resource: {cluster: true}, actions: ["fsync"]}],
            }),
        );
        assert.commandWorked(
            adminDb.runCommand({
                createUser: "fsyncUser",
                pwd: kPassword,
                roles: [{role: "fsyncOnlyRole", db: "admin"}],
            }),
        );

        const fsyncUserConn = new Mongo(st.s.host);
        fsyncUserDb = fsyncUserConn.getDB("admin");
        assert(fsyncUserDb.auth("fsyncUser", kPassword));
    });

    it("fsync-only principal cannot run fsyncUnlock", function () {
        assert.commandFailedWithCode(
            fsyncUserDb.runCommand({fsyncUnlock: 1}),
            ErrorCodes.Unauthorized,
        );
    });

    it("non-boolean lock value fails without releasing another principal's lock", function () {
        // The root user acquires the fsync lock on every node.
        let res = assert.commandWorked(adminDb.runCommand({fsync: 1, lock: true}));
        assertLockCount(res, "all", 1, "all nodes should report one held lock");

        // A string 'lock' must fail with TypeMismatch and leave the held lock in place. The
        // code is at the top level where the router parses 'lock' itself, inside 'all' where
        // the first shard error propagates.
        const stringLockRes = fsyncUserDb.runCommand({fsync: 1, lock: "x"});
        assert.commandFailed(stringLockRes);
        const stringLockCode =
            stringLockRes.code !== undefined ? stringLockRes.code : stringLockRes.all.code;
        assert.eq(
            stringLockCode,
            ErrorCodes.TypeMismatch,
            "string lock should fail with TypeMismatch",
            {stringLockRes},
        );

        // The held lock must have survived: the unlock succeeds and releases exactly the one
        // count on every node.
        res = assert.commandWorked(adminDb.runCommand({fsyncUnlock: 1}));
        assertLockCount(res, "", 0, "lock should still be held before this unlock");
    });

    it("unknown extra field does not release another principal's lock", function () {
        // The root user acquires the fsync lock on every node.
        let res = assert.commandWorked(adminDb.runCommand({fsync: 1, lock: true}));
        assertLockCount(res, "all", 1, "all nodes should report one held lock");

        // The shard-side parse rejects any field the fsync IDL does not define, so no node
        // takes a lock and the cleanup has nothing to unlock.
        const unknownFieldRes = fsyncUserDb.runCommand({
            fsync: 1,
            lock: true,
            unknownFsyncField: 1,
        });
        assert.commandFailed(unknownFieldRes);

        // The held lock must have survived.
        res = assert.commandWorked(adminDb.runCommand({fsyncUnlock: 1}));
        assertLockCount(res, "", 0, "lock should still be held before this unlock");
    });

    it("lock request from another principal joins the held lock instead of failing", function () {
        // The root user acquires the fsync lock on every node.
        assert.commandWorked(adminDb.runCommand({fsync: 1, lock: true}));

        // The fsync lock is a recursive counter: a second request while the lock is held
        // succeeds (count 1 -> 2 on every node) instead of failing, so the failed-lock cleanup
        // never runs. The acquisition timeout only applies when no lock is held yet.
        const res = assert.commandWorked(
            fsyncUserDb.runCommand({
                fsync: 1,
                lock: true,
                fsyncLockAcquisitionTimeoutMillis: NumberInt(100),
            }),
        );
        assertLockCount(res, "all", 2, "second lock should join the held lock");

        // Releasing the first count still leaves the second count held everywhere.
        let unlockRes = assert.commandWorked(adminDb.runCommand({fsyncUnlock: 1}));
        assertLockCount(unlockRes, "", 1, "one count should remain held after the first unlock");

        // The fsync-only user cannot release their own remaining count.
        assert.commandFailedWithCode(
            fsyncUserDb.runCommand({fsyncUnlock: 1}),
            ErrorCodes.Unauthorized,
        );

        // The root user releases the last count.
        unlockRes = assert.commandWorked(adminDb.runCommand({fsyncUnlock: 1}));
        assertLockCount(unlockRes, "", 0, "no counts should remain after root's unlock");

        const finalRes = adminDb.runCommand({fsyncUnlock: 1});
        assert.commandFailedWithCode(finalRes, ErrorCodes.IllegalOperation);
        assert(
            finalRes.errmsg.includes("fsyncUnlock called when not locked"),
            "unexpected errmsg",
            {finalRes},
        );
    });
});
