/**
 * Checks whether the unified write executor is used for sharded writes.
 */
export function isUweEnabled(db) {
    return !!assert.commandWorkedOrFailedWithCode(
        db.adminCommand({
            getParameter: 1,
            featureFlagUnifiedWriteExecutor: 1,
        }),
        // Allow the error when the query knob is not present.
        ErrorCodes.InvalidOptions,
    ).featureFlagUnifiedWriteExecutor;
}

/**
 * Checks whether the unified write executor uses a different write command name on the shards.
 */
export function isUweShardCmdNameChanged(cmdName) {
    return ["insert", "update", "delete", "remove"].includes(cmdName);
}

/**
 * Maps the write command name that the unified write executor uses on the shards.
 */
export function mapUweShardCmdName(cmdName) {
    if (isUweShardCmdNameChanged(cmdName)) {
        return "bulkWrite";
    }
    return cmdName;
}
