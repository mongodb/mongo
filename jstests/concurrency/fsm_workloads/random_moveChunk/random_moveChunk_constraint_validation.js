/**
 * Tests concurrent upgrading/downgrading between 'strict' and 'constraint' validation levels
 * while chunk migrations run in parallel. This exercises the document scan inside
 * prepareConstraintValidationLevel and upgradeToConstraint racing with ongoing data movement.
 *
 * Happy path: writeValidDoc → prepareConstraint → upgradeToConstraint → downgradeToStrict → loop.
 * Chunk migrations are interleaved only with the upgrade/downgrade states.
 *
 * writeInvalidDoc uses bypassDocumentValidation to land a real non-conforming document in the
 * collection, letting the scan inside prepareConstraint / upgradeToConstraint find it and return
 * error 12370902. The scan-failure handler deletes the offending doc before returning.
 *
 * @tags: [
 *  requires_sharding,
 *  assumes_balancer_off,
 *  requires_fcv_90,
 *  # bypassDocumentValidation inside a multi-statement transaction is aborted as
 *  # TransientTransactionError when prepareConstraintValidationLevel is set, causing infinite
 *  # retries in the FSM transaction retry loop.
 *  does_not_support_transactions,
 *  # cleanupInvalidDocs uses deleteMany, which is a non-retryable write. Stepdown suites
 *  # refuse to run tests that issue non-retryable writes.
 *  requires_non_retryable_writes,
 *  # collMod scan operations can time out under fuzzed storage/runtime configs.
 *  does_not_support_config_fuzzer,
 *  # TODO(SERVER-119777): Ensure test does not leak cursors.
 *  can_leak_idle_cursors,
 * ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {ConcurrentOperation} from "jstests/concurrency/fsm_workload_helpers/cluster_scalability/chunk_operation_errors.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/random_moveChunk/random_moveChunk_base.js";

const kValidator = {a: {$type: "number"}};

// Returned when the document scan inside upgradeToConstraint finds a doc
// that violates the validator.
const kScanViolationCode = 12370902;

// Error codes that are acceptable for any collMod state in a concurrent FSM test.
// ConflictingOperationInProgress fires when two threads issue collMod with different parameters
// simultaneously; the FSM's own iteration loop provides the retry.
const kCollModAcceptableCodes = [
    ErrorCodes.BadValue,
    ErrorCodes.LockTimeout,
    ErrorCodes.ConflictingOperationInProgress,
];

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.threadCount = 4;
    $config.iterations = 64;
    $config.data.partitionSize = 1000;
    $config.passConnectionCache = true;

    $config.data.getConcurrentOperations = () => [ConcurrentOperation.ValidationLevelChange];

    // -------------------------------------------------------------------------
    // States
    // -------------------------------------------------------------------------

    $config.states.writeValidDoc = function writeValidDoc(db, collName, connCache) {
        // Insert a conforming doc with bypassDocumentValidation to verify that bypass is rejected
        // once the prepare flag is set or the collection is at constraint level. When bypass is
        // allowed (strict level, prepare flag not set), the insert succeeds normally.
        const res = db.runCommand({
            insert: collName,
            documents: [
                {
                    _id: new ObjectId(),
                    skey: Random.randInt(this.partitionSize * this.threadCount),
                    a: Random.randInt(100),
                    tid: this.tid,
                },
            ],
            bypassDocumentValidation: true,
        });
        if (!res.ok) {
            // BadValue: bypass blocked because prepare flag is set or collection is at constraint.
            assert(res.code === ErrorCodes.BadValue, "Unexpected error in writeValidDoc", {res});
        }
    };

    $config.states.writeInvalidDoc = function writeInvalidDoc(db, collName, connCache) {
        // Use bypassDocumentValidation to land a non-conforming doc so that a subsequent scan in
        // prepareConstraint or upgradeToConstraint can find it and return kScanViolationCode.
        //
        // Bypass is blocked (BadValue) when the prepare flag is set or when the collection is at
        // constraint level, so both of those are expected failures.
        const doc = {
            _id: new ObjectId(),
            skey: Random.randInt(this.partitionSize * this.threadCount),
            a: "INVALID",
            tid: this.tid,
        };
        const res = db.runCommand({
            insert: collName,
            documents: [doc],
            bypassDocumentValidation: true,
        });
        if (!res.ok || res.n !== 1) {
            // Top-level failure (bypass blocked at collection level) or a write error.
            const code = res.code !== undefined ? res.code : res.writeErrors?.[0]?.code;
            const kExpected = [ErrorCodes.BadValue, ErrorCodes.DocumentValidationFailure];
            assert(kExpected.includes(code), "Unexpected error from bypassed insert", {res});
        }
    };

    // Delete all docs that violate the validator. Called after a scan failure so the collection is
    // clean for the next upgradeToConstraint attempt. Uses deleteMany because multiple threads may
    // have each planted their own invalid doc concurrently.
    $config.data.cleanupInvalidDocs = function cleanupInvalidDocs(db, collName) {
        db[collName].deleteMany({a: {$not: {$type: "number"}}});
    };

    $config.states.prepareConstraint = function prepareConstraint(db, collName, connCache) {
        const res = db.runCommand({collMod: collName, prepareConstraintValidationLevel: true});
        if (!res.ok) {
            assert(
                kCollModAcceptableCodes.includes(res.code),
                "Unexpected error in prepareConstraint",
                {res},
            );
        }
    };

    $config.states.upgradeToConstraint = function upgradeToConstraint(db, collName, connCache) {
        const res = db.runCommand({collMod: collName, validationLevel: "constraint"});
        if (!res.ok) {
            if (res.code === kScanViolationCode) {
                this.cleanupInvalidDocs(db, collName);
                return;
            }
            assert(
                kCollModAcceptableCodes.includes(res.code),
                "Unexpected error in upgradeToConstraint",
                {res},
            );
        }
    };

    $config.states.downgradeToStrict = function downgradeToStrict(db, collName, connCache) {
        const res = db.runCommand({collMod: collName, validationLevel: "strict"});
        if (!res.ok) {
            assert(
                kCollModAcceptableCodes.includes(res.code),
                "Unexpected error in downgradeToStrict",
                {res},
            );
        }
    };

    $config.states.unsetPrepare = function unsetPrepare(db, collName, connCache) {
        const res = db.runCommand({collMod: collName, prepareConstraintValidationLevel: false});
        if (!res.ok) {
            assert(kCollModAcceptableCodes.includes(res.code), "Unexpected error in unsetPrepare", {
                res,
            });
        }
    };

    // -------------------------------------------------------------------------
    // Transitions
    // -------------------------------------------------------------------------

    $config.transitions = {
        init: {writeValidDoc: 1.0},

        writeValidDoc: {
            writeValidDoc: 0.5,
            writeInvalidDoc: 0.1,
            prepareConstraint: 0.22,
            upgradeToConstraint: 0.05, // intentional: upgrade without prepare → BadValue
            downgradeToStrict: 0.08, // no-op if already strict; tolerated
            unsetPrepare: 0.05,
        },

        writeInvalidDoc: {
            upgradeToConstraint: 0.4, // dominant: scan finds bad doc → kScanViolationCode
            prepareConstraint: 0.3, // dominant: scan finds bad doc → kScanViolationCode
            writeValidDoc: 0.2,
            writeInvalidDoc: 0.05,
            downgradeToStrict: 0.05,
        },

        prepareConstraint: {
            upgradeToConstraint: 0.7, // happy-path: prepare set, now upgrade
            writeValidDoc: 0.1,
            writeInvalidDoc: 0.05,
            unsetPrepare: 0.05, // occasionally bail out without upgrading
            moveChunk: 0.1, // migration races with prepare scan
        },

        upgradeToConstraint: {
            downgradeToStrict: 0.6,
            moveChunk: 0.25, // migration races with upgrade scan
            writeValidDoc: 0.1,
            writeInvalidDoc: 0.05,
        },

        downgradeToStrict: {
            prepareConstraint: 0.4, // immediately start the cycle again
            moveChunk: 0.3, // migration races with downgrade
            writeValidDoc: 0.2,
            writeInvalidDoc: 0.1,
        },

        unsetPrepare: {
            writeValidDoc: 0.55,
            writeInvalidDoc: 0.1,
            prepareConstraint: 0.35,
        },

        moveChunk: {
            upgradeToConstraint: 0.3,
            downgradeToStrict: 0.3,
            prepareConstraint: 0.2,
            writeValidDoc: 0.2,
        },
    };

    // -------------------------------------------------------------------------
    // Setup / Teardown
    // -------------------------------------------------------------------------

    $config.setup = function setup(db, collName, cluster) {
        // Base creates the sharded collection and inserts partitioned docs as {_id, skey, tid}.
        $super.setup.apply(this, arguments);

        // Seed `a: 0` on every existing doc so they all satisfy the validator from the start.
        assert.commandWorked(
            db.runCommand({
                update: collName,
                updates: [{q: {}, u: {$set: {a: 0}}, multi: true}],
            }),
        );

        // Apply the validator. All existing docs now conform.
        assert.commandWorked(
            db.runCommand({
                collMod: collName,
                validator: kValidator,
                validationLevel: "strict",
                validationAction: "error",
            }),
        );
    };

    $config.teardown = function teardown(db, collName, cluster) {
        // Best-effort cleanup: bring the collection to a known state regardless of where threads
        // stopped. Ignore failures — threads may have left the collection in any intermediate state.
        db.runCommand({collMod: collName, prepareConstraintValidationLevel: false});
        db.runCommand({collMod: collName, validationLevel: "strict"});

        // Remove any invalid docs that threads planted but never cleaned up (e.g. because no scan
        // failure was triggered before the run ended).
        this.cleanupInvalidDocs(db, collName);

        assert.commandWorked(db.runCommand({validate: collName}));

        $super.teardown.apply(this, arguments);
    };

    return $config;
});
