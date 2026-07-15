/**
 * Container ops (ci/cd) are internal-only. _applyOps() gates them based on oplogApplicationMode:
 *   - oplogApplicationMode == kApplyOpsCmd (a direct applyOps invocation, e.g. from a client):
 *     the primary-driven-index-builds feature flag must be enabled. This branch is unchanged from
 *     the pre-fix behavior;
 *   - every other oplogApplicationMode (Secondary, InitialSync, Recovering) is entirely
 *     client-controlled and must not be trusted to indicate real oplog application, so the op is
 *     only permitted when opCtx->writesAreReplicated() is false, i.e. this is genuinely not a
 *     user-issued applyOps.
 *
 * This is the regression test for the bypass where a user-issued applyOps could set
 * oplogApplicationMode to any non-kApplyOpsCmd value (e.g. "Secondary") to skip the feature-flag
 * check entirely and issue a raw storage write. It exercises both container op types this branch
 * supports (ci/cd), since the gate covers each of them.
 */
import {afterEach, describe, it} from "jstests/libs/mochalite.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

// The internal oplog-application modes that map to something other than kApplyOpsCmd. "ApplyOps"
// is intentionally excluded: OplogApplication::parseMode("ApplyOps") == kApplyOpsCmd, i.e. it is
// the direct-command mode, not a replay mode, so it takes the feature-flag branch below rather
// than the writesAreReplicated() branch this test exercises.
const REPLAY_MODES = ["Secondary", "InitialSync", "Recovering"];

// The internal-only container op types the gate covers on this branch: insert and delete. (This
// branch has no container-update op type.)
const CONTAINER_OP_TYPES = ["ci", "cd"];

// Builds an applyOps container op of the given type, targeting an arbitrary storage ident. Every
// assertion below expects the op to be rejected by the gate before any storage access is
// attempted, so the ident never has to exist. The "o" field matches each op's schema (insert
// carries a value; delete carries only the key).
function makeContainerOp(opType) {
    const key = NumberLong("777777");
    const value = BinData(0, "AQIDBA==");
    const op = {op: opType, ns: "admin.$container", container: "index-does-not-exist"};
    switch (opType) {
        case "ci":
            op.o = {k: key, v: value};
            break;
        case "cd":
            op.o = {k: key};
            break;
        default:
            throw new Error("unknown container op type: " + opType);
    }
    return op;
}

// Asserts that every container op type (ci/cd) sent via applyOps is rejected under every replay
// mode a client could claim (Secondary, InitialSync, Recovering), since the gate must hold
// regardless of op type or mode. Also checks the default (kApplyOpsCmd) mode unless
// includeDefaultMode is false -- pass false when the feature flag is enabled, since kApplyOpsCmd is
// then legitimately allowed through (see the top-of-file comment).
function assertContainerOpsRejectedForAllModes(db, {includeDefaultMode = true} = {}) {
    const assertRejected = (opType, oplogApplicationMode) => {
        const cmd = {applyOps: [makeContainerOp(opType)]};
        if (oplogApplicationMode !== undefined) {
            cmd.oplogApplicationMode = oplogApplicationMode;
        }
        assert.commandFailedWithCode(
            db.adminCommand(cmd),
            ErrorCodes.InvalidOptions,
            "a user-issued applyOps container op must be rejected regardless of op type or oplogApplicationMode",
            {opType, oplogApplicationMode},
        );
    };

    for (const opType of CONTAINER_OP_TYPES) {
        if (includeDefaultMode) {
            assertRejected(opType);
        }
        for (const mode of REPLAY_MODES) {
            assertRejected(opType, mode);
        }
    }
}

describe("applyOps container-ops enablement gate", function () {
    afterEach(function () {
        if (this.conn) {
            MongoRunner.stopMongod(this.conn);
            this.conn = null;
        }
    });

    describe("when the primary-driven-index-builds feature is disabled", function () {
        it("rejects every user-issued container op for every oplogApplicationMode", function () {
            this.conn = MongoRunner.runMongod();
            const db = this.conn.getDB("admin");

            // This case covers the feature-disabled configuration; if a build enables the flag by
            // default there is nothing to assert here (see the feature-enabled case below).
            if (FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds")) {
                jsTest.log.info("PrimaryDrivenIndexBuilds enabled by default; skipping feature-disabled case");
                return;
            }

            assertContainerOpsRejectedForAllModes(db);
        });
    });

    describe("when the primary-driven-index-builds feature is enabled", function () {
        it("rejects every user-issued container op for every oplogApplicationMode", function () {
            this.conn = MongoRunner.runMongod({
                setParameter: {featureFlagPrimaryDrivenIndexBuilds: true},
            });
            const db = this.conn.getDB("admin");

            // If this build cannot enable the flag, there is nothing to assert.
            if (!FeatureFlagUtil.isPresentAndEnabled(db, "PrimaryDrivenIndexBuilds")) {
                jsTest.log.info("PrimaryDrivenIndexBuilds could not be enabled; skipping feature-enabled case");
                return;
            }

            // With the feature flag on, the default (kApplyOpsCmd) mode is legitimately allowed
            // through here -- exercise only the replay modes, which is what this regression test is
            // actually about.
            assertContainerOpsRejectedForAllModes(db, {includeDefaultMode: false});
        });
    });
});
