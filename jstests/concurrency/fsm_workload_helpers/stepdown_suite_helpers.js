// Commands that are not fully retryable will be refused by
// jstests/libs/override_methods/network_error_and_txn_override.js unless opting out of automatic
// retries. Use this helper to temporarily disable automatic retries for your command and handle the
// consequences of failure some other way.
export function withSkipRetryOnNetworkError(fn) {
    const previousSkipRetryOnNetworkError = TestData.skipRetryOnNetworkError;
    TestData.skipRetryOnNetworkError = true;

    let res = undefined;
    try {
        res = fn();
    } finally {
        TestData.skipRetryOnNetworkError = previousSkipRetryOnNetworkError;
    }

    return res;
}

export function inNonTransactionalStepdownSuite() {
    return TestData.runningWithShardStepdowns && !TestData.runInsideTransaction;
}

// See the comment on withSkipRetryOnNetworkError for why manual retries are necessary in stepdown
// suites. If we are also running inside a transaction, fsm.js will implicitly use
// auto_retry_transaction.js, which has its own retry logic.
export function runWithManualRetriesIfInNonTransactionalStepdownSuite(fn) {
    if (inNonTransactionalStepdownSuite()) {
        return runWithManualRetries(fn);
    } else {
        return fn();
    }
}

export function runWithManualRetries(fn) {
    let result = undefined;
    assert.soonNoExcept(() => {
        result = withSkipRetryOnNetworkError(fn);
        return true;
    });
    return result;
}

// The getMore command is not retryable, so it is not allowed to be run in suites with
// stepdown/kill/terminate. This will find only one batch to avoid calling getMore; ensure that
// the batchSize is large enough for the number of documents expected to be returned.
export function findFirstBatch(db, collName, filter, batchSize) {
    const cursor = assert.commandWorked(db.runCommand({find: collName, filter, batchSize})).cursor;
    // The helper deliberately reads a single batch to avoid a non-retryable getMore. If results
    // remain the server returns a non-zero cursor id; kill it so the test never leaks an idle
    // cursor, which can pin range deletions and hang migrations. Best-effort: killCursors is
    // allowed in stepdown suites and a failure here is harmless.
    if (cursor.id != 0) {
        db.runCommand({killCursors: collName, cursors: [cursor.id]});
    }
    return cursor.firstBatch;
}
