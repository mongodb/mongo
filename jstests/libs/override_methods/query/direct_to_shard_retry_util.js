/**
 * Shared retry logic for the direct-to-shard override files: retries StaleConfig and any
 * RetriableError code, which mongos would otherwise absorb transparently for a normal client.
 */
function isRetryableCode(code) {
    return code === ErrorCodes.StaleConfig || ErrorCodes.isRetriableError(code);
}

function hasOnlyDuplicateKeyWriteErrors(res) {
    return (
        Array.isArray(res.writeErrors) &&
        res.writeErrors.length > 0 &&
        res.writeErrors.every((writeError) => writeError.code === ErrorCodes.DuplicateKey)
    );
}

function hasRetryableWriteError(res) {
    return Array.isArray(res.writeErrors) && res.writeErrors.some((we) => isRetryableCode(we.code));
}

export function isRetryableDirectShardResult(cmdName, res) {
    // A client can't know whether the previous attempt already advanced the cursor (or already
    // consumed a one-shot failpoint/error injection), so getMore is never safe to retry here.
    if (!res || cmdName === "getMore") {
        return false;
    }
    if (isRetryableCode(res.code)) {
        return true;
    }
    // Retrying a batch with writeErrors risks double-applying items that already succeeded
    // before the failing one. This is only safe for a single-statement-shaped "insert" retry,
    // where a DuplicateKey on the next attempt (see below) tells us the item already applied.
    return cmdName === "insert" && hasRetryableWriteError(res);
}

export function retryDirectShardCommand(cmdName, sendCommand) {
    let res;
    let attempt = 0;
    assert.soon(
        () => {
            attempt++;
            res = sendCommand();
            if (attempt > 1 && cmdName === "insert" && hasOnlyDuplicateKeyWriteErrors(res)) {
                // The previous attempt's retryable error may have been returned after the insert
                // itself already applied; a DuplicateKey on retry confirms that it did.
                res = {ok: 1, n: res.writeErrors.length};
            }
            return !isRetryableDirectShardResult(cmdName, res);
        },
        () => "Timed out retrying transient error for direct-shard command: " + tojson(res),
    );
    return res;
}
