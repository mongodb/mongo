/**
 * Utility methods for reading planCache counters
 */

export function getPlanCacheSize(db) {
    return db.serverStatus().metrics.query.planCache.totalSizeEstimateBytes;
}

export function getPlanCacheNumEntries(db) {
    return db.serverStatus().metrics.query.planCache.totalQueryShapes;
}
