/**
 * Utility for reading the proactiveForeignViewResolutions serverStatus metric, which is incremented
 * each time a shard resolves a foreign view remotely via _shardsvrResolveView under
 * featureFlagExtensionsInsideHybridSearch.
 */

/**
 * Sums the proactiveForeignViewResolutions counter across the given list of shards.
 */
export function getProactiveResolutionTotal(shards) {
    let total = 0;
    for (const shard of shards) {
        const m = shard.getDB("admin").serverStatus().metrics;
        const v =
            m &&
            m.query &&
            m.query.extensionsInsideHybridSearch &&
            m.query.extensionsInsideHybridSearch.proactiveForeignViewResolutions;
        total += v || 0;
    }
    return total;
}
