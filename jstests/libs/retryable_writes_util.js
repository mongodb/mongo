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

    return {isRetryableWriteCmdName: isRetryableWriteCmdName};
})();
