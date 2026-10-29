/**
 * Helpers for running setFeatureCompatibilityVersion in tests where the transition must ultimately
 * succeed, but may transiently fail while an unrelated index build is draining.
 *
 * As of SERVER-125400, a createIndexes command pins an operation FCV for the lifetime of its
 * (possibly two-phase) index build and marks it long-running. setFCV's drain barrier
 * (_waitForOperationsRelyingOnStaleFcvToComplete) fails fast with
 * BackgroundOperationInProgressForNamespace (12587) if it observes such an in-flight build holding
 * a stale operation FCV. The build's coordinator thread is short-lived, so retrying the transition
 * succeeds once it drains. This is expected on both the upgrade and downgrade paths, since both run
 * the drain.
 */

// Upper bound on how long to keep retrying before giving up. Index builds observed by the drain at
// these call sites are short-lived, so this is a generous safety margin.
const kDefaultSetFCVRetryTimeoutMS = 5 * 60 * 1000;

// A previous transition stopped while cleaning up internal server metadata. The server refuses to
// move FCV in the opposite direction until that transition is driven to completion:
//   7428200  - a downgrade stopped mid-cleanup; it must finish before any upgrade.
//   10778001 - an upgrade stopped mid-cleanup; it must finish before any downgrade.
const kIncompleteTransitionCodes = [7428200, 10778001];

/**
 * Runs setFeatureCompatibilityVersion(targetFCV), retrying while it fails with
 * BackgroundOperationInProgressForNamespace, and completing a half-finished transition in the
 * opposite direction if one is blocking us. Any other failure fails the test immediately, so this
 * does not mask unexpected setFCV errors.
 *
 * @param {Mongo|DB} conn - a connection (Mongo) or any database handle (DB); the command is always
 *     routed to the 'admin' database.
 * @param {string} targetFCV - target feature compatibility version.
 * @param {Object} [extraFields] - additional fields merged into the command (e.g.
 *     {fromConfigServer: true}). 'confirm: true' is always included.
 * @param {number} [timeoutMS] - how long to keep retrying (default 5 minutes).
 * @returns {Object} the successful command response.
 */
export function setFCVWithRetryOnBackgroundOpInProgress(
    conn,
    targetFCV,
    extraFields = {},
    timeoutMS = kDefaultSetFCVRetryTimeoutMS,
) {
    // Resolve the admin database from either a Mongo connection or a DB handle.
    const adminDB =
        typeof conn.getSiblingDB === "function" ? conn.getSiblingDB("admin") : conn.getDB("admin");
    const cmd = Object.assign(
        {setFeatureCompatibilityVersion: targetFCV, confirm: true},
        extraFields,
    );

    let res;
    assert.soon(
        () => {
            res = adminDB.runCommand(cmd);
            if (res.ok) {
                return true;
            }

            if (res.code === ErrorCodes.BackgroundOperationInProgressForNamespace) {
                jsTest.log.info(
                    "Retrying setFeatureCompatibilityVersion after BackgroundOperationInProgressForNamespace",
                    {targetFCV, res},
                );
                return false;
            }

            if (kIncompleteTransitionCodes.includes(res.code)) {
                // Drive the interrupted transition to completion before retrying ours. Take the
                // direction from the FCV document rather than assuming lastLTS -- the pending
                // target may be lastContinuous.
                const fcvDoc = adminDB["system.version"].findOne({
                    _id: "featureCompatibilityVersion",
                });
                const pendingFCV = fcvDoc && fcvDoc.targetVersion;
                assert(
                    pendingFCV,
                    "setFCV reported an incomplete transition but the FCV document has no" +
                        " targetVersion",
                    {targetFCV, res, fcvDoc},
                );
                jsTest.log.info("Completing an interrupted FCV transition before retrying", {
                    targetFCV,
                    pendingFCV,
                    res,
                });
                // Best effort: if this attempt fails (e.g. a draining index build), the retry loop
                // observes the same incomplete-transition error and tries again.
                adminDB.runCommand(
                    Object.assign(
                        {setFeatureCompatibilityVersion: pendingFCV, confirm: true},
                        extraFields,
                    ),
                );
                return false;
            }

            assert(false, "Unexpected failure from setFeatureCompatibilityVersion", {
                targetFCV,
                res,
            });
        },
        () =>
            `Timed out retrying setFeatureCompatibilityVersion(${targetFCV}) after in-flight` +
            ` index builds failed to drain, last response: ${tojson(res)}`,
        timeoutMS,
    );
    return res;
}
