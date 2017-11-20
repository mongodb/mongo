/**
 * Utilities for testing retryable writes.
 */
var RetryableWritesUtil = (function() {
    const retryableWriteCommands =
        new Set(["delete", "findandmodify", "findAndModify", "insert", "update"]);

    /**
     * Returns true if the command name is that of a retryable write command.
     */
    function isRetryableWriteCmdName(cmdName) {
        return retryableWriteCommands.has(cmdName);
    }

    const kStorageEnginesWithoutDocumentLocking = new Set(["ephemeralForTest", "mmapv1"]);

    /**
     * Returns true if the given storage engine supports retryable writes (i.e. supports
     * document-level locking).
     */
    function storageEngineSupportsRetryableWrites(storageEngineName) {
        return !kStorageEnginesWithoutDocumentLocking.has(storageEngineName);
    }

    return {isRetryableWriteCmdName, storageEngineSupportsRetryableWrites};
})();
