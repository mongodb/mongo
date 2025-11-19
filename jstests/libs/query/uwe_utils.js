/**
 * Check whether unified write executor is used for sharded writes.
 */
export function isUweEnabled(db) {
    return db.adminCommand({
        getParameter: 1,
        internalQueryUnifiedWriteExecutor: 1,
    }).internalQueryUnifiedWriteExecutor;
}
