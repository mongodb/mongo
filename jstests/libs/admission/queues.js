/**
 * Helpers for waiting for operations to enter and leave admission queues.
 */

export const AdmissionQueue = Object.freeze({
    Ingress: "ingress",
    IngressRequest: "ingress_request",
    Execution: "execution",
    WriteThrottle: "writeThrottle",
});

/** Default bound and polling interval for the waiters below. */
const kDefaultTimeoutMS = 30000;
const kDefaultIntervalMS = 200;

/**
 * Runs $currentOp over all users' operations as reported by the node itself, which is what lets a
 * caller observe an operation another connection is holding at an admission gate.
 */
function getOperations(db, filter) {
    return db
        .getSiblingDB("admin")
        .aggregate([{$currentOp: {allUsers: true, localOps: true}}, {$match: filter}])
        .toArray();
}

/** Returns the operations tagged with `comment`, whether or not they are waiting at a gate. */
export function getOperationsByComment(db, comment) {
    return getOperations(db, {"command.comment": comment});
}

/** Returns the operations tagged with `comment` that are currently waiting in `queueName`. */
export function getOperationsInQueue(db, comment, queueName) {
    return getOperations(db, {"command.comment": comment, "currentQueue.name": queueName});
}

/**
 * Waits for an operation tagged with `comment` to be waiting in `queueName` and returns it.
 *
 * Network errors are retried, so this is safe to call against a topology whose connections can drop
 * while the operation is held back.
 */
export function waitForOperationToEnterQueue(
    db,
    comment,
    queueName,
    {timeoutMS = kDefaultTimeoutMS, intervalMS = kDefaultIntervalMS} = {},
) {
    let queuedOp;
    assert.soonRetryOnNetworkErrors(
        () => {
            const ops = getOperationsInQueue(db, comment, queueName);
            if (ops.length === 0) {
                return false;
            }
            queuedOp = ops[0];
            return true;
        },
        `expected the operation tagged '${comment}' to appear in $currentOp with ` +
            `currentQueue.name == '${queueName}'`,
        timeoutMS,
        intervalMS,
    );
    return queuedOp;
}

/**
 * Waits for an operation tagged with `comment` to be running while waiting at no gate at all, and
 * returns it. Its `queues` breakdown then reports what its finished admissions recorded, rather
 * than a snapshot of a wait still in progress.
 *
 * Network errors are retried, as for waitForOperationToEnterQueue.
 */
export function waitForOperationToLeaveQueue(
    db,
    comment,
    {timeoutMS = kDefaultTimeoutMS, intervalMS = kDefaultIntervalMS} = {},
) {
    let admittedOp;
    assert.soonRetryOnNetworkErrors(
        () => {
            const ops = getOperationsByComment(db, comment);
            if (ops.length === 0 || ops[0].currentQueue !== null) {
                return false;
            }
            admittedOp = ops[0];
            return true;
        },
        `expected the operation tagged '${comment}' to be past every admission gate`,
        timeoutMS,
        intervalMS,
    );
    return admittedOp;
}
