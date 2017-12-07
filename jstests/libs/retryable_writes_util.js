/**
 * Utilities for testing retryable writes.
 */
var RetryableWritesUtil = (function() {
    /**
     * Returns true if the error code is retryable, assuming the command is idempotent.
     */
    function isRetryableCode(code) {
        return ErrorCodes.isNetworkError(code) || ErrorCodes.isNotMasterError(code) ||
            ErrorCodes.isWriteConcernError(code) || ErrorCodes.isInterruption(code);
    }

    const kRetryableWriteCommands =
        new Set(["delete", "findandmodify", "findAndModify", "insert", "update"]);

    /**
     * Returns true if the command name is that of a retryable write command.
     */
    function isRetryableWriteCmdName(cmdName) {
        return kRetryableWriteCommands.has(cmdName);
    }

    const kStorageEnginesWithoutDocumentLocking = new Set(["ephemeralForTest", "mmapv1"]);

    /**
     * Returns true if the given storage engine supports retryable writes (i.e. supports
     * document-level locking).
     */
    function storageEngineSupportsRetryableWrites(storageEngineName) {
        return !kStorageEnginesWithoutDocumentLocking.has(storageEngineName);
    }

    return {isRetryableCode, isRetryableWriteCmdName, storageEngineSupportsRetryableWrites};
})();
