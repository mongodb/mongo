/**
 * Decorators that fan a base test-config object out into its clustered, collation, and sharded
 * variants, shared by the change-stream updateLookup and $_internalSearchIdLookup config
 * matrices (each builds its config list as config -> clustered -> collation / sharded chains).
 */

// Adds a clustered-_id-index variant of 'config'.
export function withClusteredColl(config) {
    return {
        ...config,
        name: `${config.name}, clustered`,
        collOpts: {...config.collOpts, clusteredIndex: {key: {_id: 1}, unique: true}},
    };
}

// Adds a non-simple-collation variant of 'config'.
export function withCollation(config) {
    return {
        ...config,
        name: `${config.name}, with collation`,
        collOpts: {...config.collOpts, collation: {locale: "en"}},
    };
}

// Adds the hashed and range sharding variants of 'config' (its 'key' holds the shard-key spec).
export function withShardedColl(config) {
    const shardKey = Object.keys(config.key)[0];
    const shardedByHash = {
        ...config,
        name: `${config.name}, hashed`,
        key: {[shardKey]: "hashed"},
    };
    const shardedByRange = {
        ...config,
        name: `${config.name}, range`,
        key: {[shardKey]: 1},
    };
    return [shardedByHash, shardedByRange];
}
