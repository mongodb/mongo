/**
 * Verifies the "recently initiated" initial-sync optimization with its default threshold (0), the
 * way it is exercised in production: a replica set created with an all-at-once replSetInitiate
 * (every member listed in the initiate config, all starting empty).
 *
 *
 * @tags: [
 *   # All nodes must run the same binary. The all-at-once replSetInitiate quorum check contacts
 *   # all members; a newer binary rejects connections from older-binary nodes with incompatible
 *   # wire versions, causing the quorum check to fail immediately.
 *   multiversion_incompatible,
 * ]
 */
import {kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

describe("recently-initiated initial-sync skip on a production all-at-once initiate", function () {
    let rst;
    let primary;
    let initiatingSetTs;
    let syncingMembers;

    before(function () {
        // Three empty nodes. Hang initial sync right after _checkIfInitiatingSet has run (before
        // cloning) so we can read the reset beginApplyingTimestamp while the sync source's stable
        // recovery timestamp is still pinned at the initiating-set entry.
        rst = new ReplSetTest({
            nodes: 3,
            nodeOptions: {
                setParameter: {
                    numInitialSyncAttempts: 1,
                    "failpoint.initialSyncHangBeforeCopyingDatabases": tojson({mode: "alwaysOn"}),
                },
            },
        });
        rst.startSet();

        // Production-style all-at-once initiate: replSetInitiate with the full 3-member config on
        // node 0, NOT ReplSetTest's staged (initiate node 0, reconfig the rest in) flow.
        assert.commandWorked(rst.nodes[0].adminCommand({replSetInitiate: rst.getReplSetConfig()}));

        primary = rst.nodes[0];
        assert.soon(
            () => primary.adminCommand({hello: 1}).isWritablePrimary,
            "node 0 did not become primary",
        );

        initiatingSetTs = primary
            .getDB("local")
            .oplog.rs.find()
            .sort({$natural: 1})
            .limit(1)
            .next().ts;
        syncingMembers = [rst.nodes[1], rst.nodes[2]];
    });

    it("resets beginApplyingTimestamp to the initiating-set entry with the default threshold", function () {
        // Both members are still initial-syncing, so no majority has formed and the primary's stable
        // recovery timestamp is still pinned at the initiating-set entry (diff == 0) -- the skip
        // fires with the default threshold (0), no override needed.
        //
        // The failpoint is already enabled at startup (see before()), so the members may already be
        // paused inside it by now. Wait with an absolute timesEntered of 1 rather than
        // configureFailPoint(...).wait(): a relative wait keyed off the count captured at configure
        // time would wait for a subsequent entry that never comes and hang.
        try {
            for (const m of syncingMembers) {
                assert.commandWorked(
                    m.adminCommand({
                        waitForFailPoint: "initialSyncHangBeforeCopyingDatabases",
                        timesEntered: 1,
                        maxTimeMS: kDefaultWaitForFailPointTimeout,
                    }),
                );
            }

            for (const m of syncingMembers) {
                const status = assert.commandWorked(m.adminCommand({replSetGetStatus: 1}));
                assert(status.initialSyncStatus, "expected initialSyncStatus during initial sync", {
                    host: m.host,
                });
                assert.eq(
                    status.initialSyncStatus.initialSyncOplogStart,
                    initiatingSetTs,
                    "beginApplyingTimestamp should be reset to the initiating-set entry",
                    {host: m.host, initiatingSetTs},
                );
            }
        } finally {
            for (const m of syncingMembers) {
                assert.commandWorked(
                    m.adminCommand({
                        configureFailPoint: "initialSyncHangBeforeCopyingDatabases",
                        mode: "off",
                    }),
                );
            }
        }
    });

    after(function () {
        if (rst) {
            rst.awaitSecondaryNodes();
            rst.stopSet();
        }
    });
});
