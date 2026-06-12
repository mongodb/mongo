/**
 * Test helpers to turn a collection's "allow chunk operations" flag on or off, and to read it back.
 *
 * The server has no single command for this, so the helper runs the two internal commands the
 * server itself uses, in order:
 *   1. _configsvrSetAllowChunkOperations on the config server, which updates
 *      config.collections.<ns>.allowChunkOperations.
 *   2. _shardsvrSetAllowChunkOperations, which sets the per-shard flag and waits for any in-flight
 *      split/merge/mergeAllChunks on the namespace to finish. It is broadcast to every shard in the
 *      cluster and does not take a shard version.
 *
 * Both commands are retryable writes, so each call uses a fresh session.
 */

/**
 * Build a map from shardName -> primary Mongo connection for every shard in the fixture.
 */
function buildShardConnsByName(st) {
    const byName = {};
    for (let i = 0; i < st._rs.length; i++) {
        const conn = st["shard" + i];
        byName[conn.shardName] = conn;
    }
    return byName;
}

/**
 * Sets the "allow chunk operations" flag for `ns` to `allow`, by running the config-server command
 * and then the shard command on each shard, asserting both succeed.
 */
export function setAllowChunkOperations(st, ns, allow, opts = {}) {
    const collectionUUID = opts.collectionUUID;

    // Step 1: config-server write. It is a retryable write, so use a fresh session.
    {
        const configCmd = {
            _configsvrSetAllowChunkOperations: ns,
            allowChunkOperations: allow,
            writeConcern: {w: "majority"},
            lsid: {id: UUID()},
            txnNumber: NumberLong(0),
        };
        if (collectionUUID !== undefined) {
            configCmd.collectionUUID = collectionUUID;
        }
        assert.commandWorked(st.configRS.getPrimary().adminCommand(configCmd));
    }

    // Step 2: broadcast the command to every shard in the cluster. It must be a retryable write
    const shardConnsByName = buildShardConnsByName(st);
    const shardNames = opts.shards !== undefined ? opts.shards : Object.keys(shardConnsByName);

    // The command requires the database's primary shard (it tells each shard whether it is the
    // primary and must persist metadata). It is the same value for every shard we send to.
    const dbName = ns.split(".")[0];
    const primaryShardId = st.getPrimaryShard(dbName).shardName;

    for (const shardName of shardNames) {
        const shardConn = shardConnsByName[shardName];
        assert(shardConn, "no Mongo connection for shard " + shardName);

        const shardCmd = {
            _shardsvrSetAllowChunkOperations: ns,
            allowChunkOperations: allow,
            primaryShardId: primaryShardId,
            writeConcern: {w: "majority"},
            lsid: {id: UUID()},
            txnNumber: NumberLong(0),
        };
        if (collectionUUID !== undefined) {
            shardCmd.collectionUUID = collectionUUID;
        }
        assert.commandWorked(shardConn.adminCommand(shardCmd));
    }
}

/**
 * Reads config.collections.<ns>.allowChunkOperations. Returns undefined when the field is absent,
 * which is how the config server records the "allowed" state.
 */
export function getConfigSvrAllowChunkOperations(st, ns) {
    const doc = st.s.getDB("config").collections.findOne({_id: ns});
    return doc === null ? undefined : doc.allowChunkOperations;
}

/**
 * Reads the shard's persisted allowChunkOperations value from its local
 * config.shard.catalog.collections. This is the on-disk value the shard reloads after stepping up.
 * undefined means "allowed", false means "disallowed". Reading this document is the simplest way to
 * observe the per-shard state from a test.
 */
export function getShardAllowChunkOperations(shardConn, ns) {
    const doc = shardConn
        .getDB("config")
        .getCollection("shard.catalog.collections")
        .findOne({_id: ns});
    return doc === null ? undefined : doc.allowChunkOperations;
}
