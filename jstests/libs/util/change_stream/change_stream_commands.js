/**
 * Commands for change stream configuration testing.
 * Defines all command classes that perform operations on database/collection states.
 *
 * Note: Random is a global object provided by the MongoDB shell.
 *
 * ================================================================================
 * CHANGE STREAM WATCH MODE AND INVALIDATE EVENTS
 * ================================================================================
 *
 * The `getChangeEvents(watchMode)` method requires a `watchMode` parameter
 * (ChangeStreamWatchMode) to determine which events to return.
 *
 * Invalidate behavior varies by operation and watch level:
 *
 * | Operation      | kCollection | kDb         | kCluster    |
 * |----------------|-------------|-------------|-------------|
 * | drop           | invalidate  | no          | no          |
 * | rename         | invalidate  | no          | no          |
 * | dropDatabase   | invalidate  | invalidate  | no          |
 *
 * This matches MongoDB's behavior where:
 * - Collection streams invalidate on drop/rename/dropDatabase (collection is gone)
 * - Database streams invalidate only on dropDatabase (database is gone)
 * - Cluster streams never invalidate (cluster continues to exist)
 *
 * ================================================================================
 * KEY INSIGHT: Per-Shard Event Emission for createIndexes/dropIndexes
 * ================================================================================
 *
 * When MongoDB executes createIndexes or dropIndexes on a sharded collection, each
 * shard that has data for that collection emits its own change stream event. This
 * results in multiple events with the same clusterTime (one per shard).
 *
 * The number of events depends on the CURRENT data distribution:
 *
 * | Current Data Distribution      | Events Emitted   |
 * |--------------------------------|------------------|
 * | Not sharded / single shard     | 1                |
 * | Range (single chunk initially) | 1                |
 * | Hashed (pre-split across set)  | shardSet.length  |
 *
 * WHY HASHED = shardSet.length:
 * - Hashed shard keys cause IMMEDIATE pre-split distribution across shards in the shard set.
 * - Zones are used to restrict data to only shards in the shardSet.
 * - Each shard in the shardSet emits its own createIndexes/dropIndexes event.
 *
 * WHY RANGE = 1 (initially):
 * - Range shard keys start with a SINGLE chunk on the primary shard
 * - Data only exists on one shard initially (until splits/migrations occur)
 * - Only that one shard emits the event
 *
 * IMPORTANT FOR RESHARD OPERATIONS:
 * - CreateIndexCommand (before reshard): Event count based on CURRENT shard key distribution
 *   Example: hashed→range reshard creates index while data is still hashed = N events
 * - DropIndexCommand (after reshard): Event count based on NEW shard key distribution
 *   Example: hashed→range reshard drops old index after data is range-distributed = 1 event
 *
 * LIMITATION: This logic assumes hashed keys always distribute to all shards and range
 * keys stay on one shard. In reality, data distribution can change via moveChunk/balancer.
 * TODO SERVER-114858: Track actual data distribution instead of inferring from shard key type.
 *
 * The generator passes `collectionCtx.shardKeySpec` (the collection's current shard key)
 * to commands for predicting event counts based on current data distribution.
 * ================================================================================
 */

import {ChangeStreamWatchMode} from "jstests/libs/query/change_stream_util.js";

/**
 * Sharding type constants.
 */
const ShardingType = {
    RANGE: "range",
    HASHED: "hashed",
};

/**
 * Get the shard key spec for a given sharding type.
 * Uses 'data' field which requires explicit index creation before sharding.
 * @param {string} shardingType - ShardingType.RANGE or ShardingType.HASHED
 * @returns {Object} The shard key spec, e.g. {data: 1} or {data: "hashed"}
 */
function getShardKeySpec(shardingType) {
    return shardingType === ShardingType.HASHED ? {data: "hashed"} : {data: 1};
}

/**
 * Base command class.
 */
class Command {
    constructor(dbName, collName, shardSet, collectionCtx = {}) {
        this.dbName = dbName;
        this.collName = collName;
        this.shardSet = shardSet;
        this.collectionCtx = collectionCtx;
    }

    /**
     * Execute the command.
     * @param {Object} connection - The MongoDB connection.
     */
    execute(connection) {
        throw new Error("execute() method must be implemented by subclasses");
    }

    /**
     * String representation of the command.
     */
    toString() {
        throw new Error("toString() method must be implemented by subclasses");
    }

    /**
     * Get expected change events for this command.
     * Returns an array of partial event documents to match against actual events.
     *
     * @param {number} watchMode - ChangeStreamWatchMode (kCollection, kDb, or kCluster).
     *   Required. Filters events based on watch level (e.g., omits 'invalidate' for db/cluster streams).
     * @returns {Array} Array of expected change event documents.
     */
    getChangeEvents(watchMode) {
        throw new Error("getChangeEvents() must be implemented by subclasses");
    }
}

/**
 * Insert document command.
 * The insertion behavior differs depending on whether the collection already exists.
 */
class InsertDocCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
        // Create the document in the constructor so it can be used by both execute() and getChangeEvents().
        this.document = {
            _id: new ObjectId(),
            timestamp: new Date(),
            data: `test_data`,
        };
    }

    execute(connection) {
        assert.commandWorked(connection.getDB(this.dbName).getCollection(this.collName).insertOne(this.document));
    }

    toString() {
        return "InsertDocCommand";
    }

    getChangeEvents(watchMode) {
        const events = [];
        // Inserting into non-existent collection implicitly creates it.
        if (!this.collectionCtx.exists) {
            events.push(_createCollectionEventSpec(this.dbName, this.collName));
        }
        events.push({
            operationType: "insert",
            ns: {db: this.dbName, coll: this.collName},
            fullDocument: this.document,
        });
        return events;
    }
}

/**
 * Create database command.
 */
class CreateDatabaseCommand extends Command {
    execute(connection) {
        assert.commandWorked(connection.adminCommand({enableSharding: this.dbName}));
    }

    toString() {
        return "CreateDatabaseCommand";
    }

    getChangeEvents(watchMode) {
        // enableSharding does not emit change stream events.
        return [];
    }
}

/**
 * Create unsplittable collection command.
 */
class CreateUnsplittableCollectionCommand extends Command {
    execute(connection) {
        assert(
            this.shardSet && this.shardSet.length > 0,
            "Shard set must be provided for CreateUnsplittableCollectionCommand",
        );
        const targetShard = this.shardSet[Random.randInt(this.shardSet.length)];

        const db = connection.getDB(this.dbName);
        assert.commandWorked(
            db.runCommand({
                createUnsplittableCollection: this.collName,
                dataShard: targetShard._id,
            }),
        );
    }

    toString() {
        return "CreateUnsplittableCollectionCommand";
    }

    getChangeEvents(watchMode) {
        return [_createCollectionEventSpec(this.dbName, this.collName)];
    }
}

/**
 * Create untracked collection command.
 */
class CreateUntrackedCollectionCommand extends Command {
    execute(connection) {
        assert.commandWorked(connection.getDB(this.dbName).createCollection(this.collName));
    }

    toString() {
        return "CreateUntrackedCollectionCommand";
    }

    getChangeEvents(watchMode) {
        return [_createCollectionEventSpec(this.dbName, this.collName)];
    }
}

/**
 * Drop collection command.
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * - If collection exists: emits 'drop' then 'invalidate' events (in that order).
 * - If collection does not exist: no events are emitted.
 * - 'invalidate' is only emitted for collection-level change streams (watchMode === kCollection).
 *
 * IMPORTANT: MongoDB does NOT emit 'dropIndexes' events when dropping a collection,
 * even for sharded/resharded collections with multiple indexes. The drop operation
 * implicitly removes all indexes without explicit dropIndexes events.
 * This was verified empirically through testing.
 */
class DropCollectionCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
    }

    execute(connection) {
        const db = connection.getDB(this.dbName);
        assert.commandWorked(db.runCommand({drop: this.collName}));
    }

    toString() {
        return "DropCollectionCommand";
    }

    getChangeEvents(watchMode) {
        assert(watchMode !== undefined, "watchMode must be specified for getChangeEvents()");
        // If collection doesn't exist, no events are emitted.
        if (!this.collectionCtx.exists) {
            return [];
        }
        // drop → invalidate sequence (no dropIndexes, even for sharded collections).
        // 'invalidate' is only emitted for collection-level streams.
        const events = [
            {
                operationType: "drop",
                ns: {db: this.dbName, coll: this.collName},
            },
        ];
        if (watchMode === ChangeStreamWatchMode.kCollection) {
            events.push({operationType: "invalidate"});
        }
        return events;
    }
}

/**
 * Drop database command.
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * MongoDB emits 'drop' for each collection, then 'dropDatabase' event.
 * - 'dropDatabase' event has ns: {db: dbName} (no coll field).
 * - 'invalidate' follows for collection/database level streams.
 *
 * NOTE: In our test setup, the generator drops collections BEFORE dropping the database
 * (via DropCollectionCommand), so by the time this command runs, collectionExists = false.
 * This means we only emit the 'dropDatabase' event, not individual 'drop' events.
 */
class DropDatabaseCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
    }

    execute(connection) {
        const db = connection.getDB(this.dbName);
        assert.commandWorked(db.dropDatabase());
    }

    toString() {
        return "DropDatabaseCommand";
    }

    getChangeEvents(watchMode) {
        assert(watchMode !== undefined, "watchMode must be specified for getChangeEvents()");
        const events = [];

        // If collection still exists, MongoDB emits 'drop' for it before 'dropDatabase'.
        // (In our test setup, collection is already dropped by generator, so this is usually false.)
        if (this.collectionCtx.exists) {
            events.push({
                operationType: "drop",
                ns: {db: this.dbName, coll: this.collName},
            });
        }

        // For collection-level streams: cursor is already invalidated after the collection drop
        // (either from DropCollectionCommand before this, or from the implicit drop above).
        // We won't see the 'dropDatabase' event because cursor is closed.
        if (watchMode === ChangeStreamWatchMode.kCollection) {
            if (this.collectionCtx.exists) {
                events.push({operationType: "invalidate"});
            }
            // If collection doesn't exist, we can't watch it anyway - no events.
            return events;
        }

        // For database/cluster level streams: emit 'dropDatabase' event (no coll field).
        events.push({
            operationType: "dropDatabase",
            ns: {db: this.dbName},
        });

        // 'invalidate' for database level only (database is gone). Cluster never invalidates.
        if (watchMode === ChangeStreamWatchMode.kDb) {
            events.push({operationType: "invalidate"});
        }
        return events;
    }
}

/**
 * Helper function to check if a shard key spec uses hashed sharding.
 *
 * This is important for predicting change stream event counts because:
 * - Hashed shard keys cause IMMEDIATE data distribution across participating shards (pre-split).
 * - Range shard keys start with a SINGLE chunk on one shard.
 *
 * This distribution difference affects how many createIndexes/dropIndexes events
 * are emitted - one per shard that has data.
 *
 * @param {Object} shardKeySpec - The shard key specification (e.g., {data: "hashed"} or {data: 1})
 * @returns {boolean} - True if any field in the shard key uses "hashed"
 */
function isHashedShardKey(shardKeySpec) {
    if (!shardKeySpec) {
        return false;
    }
    return Object.values(shardKeySpec).some((v) => v === "hashed");
}

/**
 * Generate per-shard events for index operations (createIndexes/dropIndexes).
 *
 * Index operations emit one event per shard that has data. For sharded collections
 * with hashed keys, data is pre-split across all shards in the shard set, so each
 * shard emits its own event. For range keys or unsharded collections, data starts
 * on a single shard.
 *
 * @param {string} operationType - The operation type ("createIndexes" or "dropIndexes").
 * @param {string} dbName - Database name.
 * @param {string} collName - Collection name.
 * @param {boolean} isSharded - Whether the collection is sharded.
 * @param {Object} currentShardKeySpec - The current shard key specification.
 * @param {Object} indexSpec - Optional index spec to use for event count (for drop index after unshard).
 * @param {Array} shardSet - Array of shards in the shard set.
 * @returns {Array} - Array of event objects.
 */
function generatePerShardIndexEvents(
    operationType,
    dbName,
    collName,
    isSharded,
    currentShardKeySpec,
    indexSpec,
    shardSet,
) {
    // TODO SERVER-114858: This logic assumes hashed keys distribute to all shards and
    // range keys stay on one shard. Once move chunk is properly implemented, we may
    // need to track actual data distribution rather than inferring from shard key type.
    // Use indexSpec if provided (for drop index), otherwise use currentShardKeySpec.
    const keySpecForEventCount = indexSpec || currentShardKeySpec;
    const hasHashedShardKey = isHashedShardKey(keySpecForEventCount);
    const numEvents = isSharded && hasHashedShardKey ? shardSet.length : 1;

    const events = [];
    for (let i = 0; i < numEvents; i++) {
        events.push({
            operationType: operationType,
            ns: {db: dbName, coll: collName},
        });
    }
    return events;
}

/**
 * Helper to create a "create" event spec for a collection.
 * @param {string} dbName - Database name.
 * @param {string} collName - Collection name.
 * @returns {Object} - The create event specification.
 */
function _createCollectionEventSpec(dbName, collName) {
    return {operationType: "create", ns: {db: dbName, coll: collName}};
}

/**
 * Helper to get the zone name for a namespace.
 */
function _getZoneName(ns) {
    return `zone_${ns.replace(".", "_")}`;
}

/**
 * Helper to configure zones to restrict a collection to specific shards.
 * This ensures data is only distributed to shards in the shardSet, not all cluster shards.
 *
 * @param {Object} connection - MongoDB connection.
 * @param {string} ns - Full namespace (db.collection).
 * @param {Object} shardKeySpec - The shard key specification.
 * @param {Array} shardSet - Array of shard objects to restrict data to.
 */
function _configureZonesForShardSet(connection, ns, shardKeySpec, shardSet) {
    const zoneName = _getZoneName(ns);
    const shardKeyField = Object.keys(shardKeySpec)[0];

    // Add all shardSet shards to the zone.
    for (const shard of shardSet) {
        assert.commandWorked(
            connection.adminCommand({
                addShardToZone: shard._id,
                zone: zoneName,
            }),
        );
    }

    // Assign the entire key range to the zone.
    assert.commandWorked(
        connection.adminCommand({
            updateZoneKeyRange: ns,
            min: {[shardKeyField]: MinKey},
            max: {[shardKeyField]: MaxKey},
            zone: zoneName,
        }),
    );
}

/**
 * Create index command.
 * Creates an index for the shard key (required before sharding or resharding).
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * The number of 'createIndexes' events emitted depends on the collection's CURRENT
 * shard key type (not the index being created):
 *
 * | Current Shard Key           | Events Emitted   |
 * |-----------------------------|------------------|
 * | Not sharded                 | 1                |
 * | Range (e.g., {a:1})         | 1                |
 * | Hashed (e.g., {a:"hashed"}) | shardSet.length  |
 *
 * WHY: Hashed shard keys cause immediate pre-split distribution across shards in the shard set
 * (via zones), so each shard emits its own createIndexes event. Range shard keys start with a
 * single chunk on one shard, so only one event is emitted.
 *
 * Precondition (guaranteed by FSM): collection exists.
 */
class CreateIndexCommand extends Command {
    /**
     * @param {string} dbName - Database name.
     * @param {string} collName - Collection name.
     * @param {Array} shardSet - Array of shard objects.
     * @param {Object} collectionCtx - Collection state (shardKeySpec = current shard key).
     * @param {Object} indexSpec - The index specification to create.
     */
    constructor(dbName, collName, shardSet, collectionCtx, indexSpec) {
        super(dbName, collName, shardSet, collectionCtx);
        assert(indexSpec, "indexSpec must be provided to CreateIndexCommand");
        this.indexSpec = indexSpec;
    }

    execute(connection) {
        const coll = connection.getDB(this.dbName).getCollection(this.collName);
        // createIndex is idempotent - succeeds silently if index already exists.
        assert.commandWorked(coll.createIndex(this.indexSpec));
    }

    toString() {
        return `CreateIndexCommand(${JSON.stringify(this.indexSpec)})`;
    }

    getChangeEvents(watchMode) {
        return generatePerShardIndexEvents(
            "createIndexes",
            this.dbName,
            this.collName,
            this.collectionCtx.isSharded || false,
            this.collectionCtx.shardKeySpec || null,
            null, // No special index spec for create
            this.shardSet,
        );
    }
}

/**
 * Drop index command.
 * Drops an old shard key index (cleanup after resharding to a new shard key).
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * The number of 'dropIndexes' events emitted depends on the collection's CURRENT
 * shard key type (the NEW shard key AFTER resharding, not the index being dropped):
 *
 * | Current Shard Key           | Events Emitted   |
 * |-----------------------------|------------------|
 * | Not sharded                 | 1                |
 * | Range (e.g., {a:1})         | 1                |
 * | Hashed (e.g., {a:"hashed"}) | shardSet.length  |
 *
 * WHY: After resharding, data distribution is determined by the NEW shard key.
 * If the new key is hashed, data is pre-split across shards in the shard set (via zones),
 * so each shard emits a dropIndexes event when removing the old index. If the new key is
 * range, data starts on a single shard, resulting in one event.
 *
 * IMPORTANT: The generator passes `collectionCtx.shardKeySpec` as the NEW shard key
 * (after reshard) for event count calculation, and `indexSpec` as the old index to drop.
 *
 * Preconditions (guaranteed by generator): collection exists, index exists.
 */
class DropIndexCommand extends Command {
    /**
     * @param {string} dbName - Database name.
     * @param {string} collName - Collection name.
     * @param {Array} shardSet - Array of shard objects.
     * @param {Object} collectionCtx - Collection state (shardKeySpec = current shard key).
     * @param {Object} indexSpec - The index specification to drop.
     */
    constructor(dbName, collName, shardSet, collectionCtx, indexSpec) {
        super(dbName, collName, shardSet, collectionCtx);
        assert(indexSpec, "indexSpec must be provided to DropIndexCommand");
        this.indexSpec = indexSpec;
    }

    execute(connection) {
        const coll = connection.getDB(this.dbName).getCollection(this.collName);
        assert.commandWorked(coll.dropIndex(this.indexSpec));
    }

    toString() {
        return `DropIndexCommand(${JSON.stringify(this.indexSpec)})`;
    }

    getChangeEvents(watchMode) {
        // Event count is based on CURRENT shard key distribution, not the index being dropped.
        // When the collection is hashed-sharded, all shards emit events for any index operation.
        return generatePerShardIndexEvents(
            "dropIndexes",
            this.dbName,
            this.collName,
            this.collectionCtx.isSharded || false,
            this.collectionCtx.shardKeySpec || null,
            null, // Use current shard key for event count, like CreateIndexCommand
            this.shardSet,
        );
    }
}

/**
 * Shard existing collection command.
 * Sharding type (range vs hashed) is determined by the shardKeySpec in collectionCtx.
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * shardCollection emits ONLY a 'shardCollection' event.
 *
 * IMPORTANT: Index creation is handled SEPARATELY by the generator:
 * - CreateIndexCommand (BEFORE shard): Creates the shard key index
 *   → Since collection is not yet sharded, emits 1 createIndexes event
 *
 * The generator orchestrates the full sequence:
 *   1. CreateIndexCommand (shard key index)
 *   2. ShardCollectionCommand (this command)
 *
 * Preconditions (guaranteed by FSM): collection exists, shard key index exists.
 */
class ShardCollectionCommand extends Command {
    /**
     * @param {string} dbName - Database name.
     * @param {string} collName - Collection name.
     * @param {Array} shardSet - Array of shard objects.
     * @param {Object} collectionCtx - Collection state.
     * @param {Object} shardKey - The shard key to use for sharding.
     */
    constructor(dbName, collName, shardSet, collectionCtx, shardKey) {
        super(dbName, collName, shardSet, collectionCtx);
        assert(shardKey, "shardKey must be provided to ShardCollectionCommand");
        this.shardKey = shardKey;
    }

    execute(connection) {
        const ns = `${this.dbName}.${this.collName}`;

        // Configure zones to restrict data to shardSet shards only.
        _configureZonesForShardSet(connection, ns, this.shardKey, this.shardSet);

        // Shard the collection with presplitHashedZones for hashed keys.
        // Note: presplitHashedZones only works when collection is empty.
        const shardCmd = {
            shardCollection: ns,
            key: this.shardKey,
        };
        if (isHashedShardKey(this.shardKey) && !this.collectionCtx.nonEmpty) {
            shardCmd.presplitHashedZones = true;
        }
        assert.commandWorked(connection.adminCommand(shardCmd));
    }

    toString() {
        const type = isHashedShardKey(this.shardKey) ? "hashed" : "range";
        return `ShardCollectionCommand(${type})`;
    }

    getChangeEvents(watchMode) {
        const events = [];

        // When sharding a non-existent collection, MongoDB implicitly:
        // 1. Creates the collection (emits 'create')
        // 2. Creates the shard key index (emits 'createIndexes')
        // 3. Shards the collection (emits 'shardCollection')
        if (!this.collectionCtx.exists) {
            events.push({
                operationType: "create",
                ns: {db: this.dbName, coll: this.collName},
            });
            events.push({
                operationType: "createIndexes",
                ns: {db: this.dbName, coll: this.collName},
            });
        }

        events.push({
            operationType: "shardCollection",
            ns: {db: this.dbName, coll: this.collName},
        });

        return events;
    }
}

/**
 * Unshard collection command.
 * Converts a sharded collection to an unsplittable (single-shard) collection.
 * Picks a random shard as the destination.
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * unshardCollection emits ONLY a 'reshardCollection' event.
 *
 * NOTE: The old shard key index is dropped AFTER unshardCollection by the generator
 * (similar to reshardCollection cleanup). This ensures:
 * - Clean state without unnecessary indexes on unsplittable collections
 * - Consistency with reshardCollection behavior
 * - No issues if the collection is resharded later with a different key
 *
 * The generator orchestrates the full sequence:
 *   1. UnshardCollectionCommand (this command)
 *   2. DropIndexCommand (old shard key index cleanup)
 *
 * Precondition (guaranteed by FSM): collection exists and is sharded.
 */
class UnshardCollectionCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
    }

    execute(connection) {
        assert(this.shardSet && this.shardSet.length > 0, "Shard set must be provided for UnshardCollectionCommand");
        const targetShard = this.shardSet[Random.randInt(this.shardSet.length)];
        const ns = `${this.dbName}.${this.collName}`;
        assert.commandWorked(
            connection.adminCommand({
                unshardCollection: ns,
                toShard: targetShard._id,
            }),
        );
    }

    toString() {
        return "UnshardCollectionCommand";
    }

    getChangeEvents(watchMode) {
        // unshardCollection internally uses reshard machinery to move data to a single shard,
        // so MongoDB emits a reshardCollection event (not an unshardCollection event).
        // Note: dropIndexes event for the old shard key is handled by a separate DropIndexCommand.
        return [
            {
                operationType: "reshardCollection",
                ns: {db: this.dbName, coll: this.collName},
            },
        ];
    }
}

/**
 * Reshard collection command.
 * Changes the shard key of an already-sharded collection.
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * reshardCollection emits ONLY a 'reshardCollection' event.
 *
 * IMPORTANT: Index-related events are handled SEPARATELY by the generator:
 * - CreateIndexCommand (BEFORE reshard): Creates index for new shard key
 *   → Event count depends on OLD shard key type (see CreateIndexCommand docs)
 * - DropIndexCommand (AFTER reshard): Drops old shard key index
 *   → Event count depends on NEW shard key type (see DropIndexCommand docs)
 *
 * The generator orchestrates the full sequence:
 *   1. CreateIndexCommand (new shard key index)
 *   2. ReshardCollectionCommand (this command)
 *   3. DropIndexCommand (old shard key index cleanup)
 *
 * Precondition (guaranteed by FSM): collection exists and is sharded, index exists.
 */
class ReshardCollectionCommand extends Command {
    // Number of initial chunks for resharding. Set to low value (1) for testing to avoid
    // cardinality errors with minimal test data. Can be modified externally if needed.
    static numInitialChunks = 1;

    /**
     * @param {string} dbName - Database name.
     * @param {string} collName - Collection name.
     * @param {Array} shardSet - Array of shard objects.
     * @param {Object} collectionCtx - Collection state.
     * @param {Object} newShardKey - The new shard key to reshard to.
     */
    constructor(dbName, collName, shardSet, collectionCtx, newShardKey) {
        super(dbName, collName, shardSet, collectionCtx);
        assert(newShardKey, "newShardKey must be provided to ReshardCollectionCommand");
        this.newShardKey = newShardKey;
    }

    execute(connection) {
        const ns = `${this.dbName}.${this.collName}`;

        // Update zones for the new shard key to restrict data to shardSet shards.
        _configureZonesForShardSet(connection, ns, this.newShardKey, this.shardSet);

        // Build the zones array for reshardCollection.
        const zoneName = _getZoneName(ns);
        const shardKeyField = Object.keys(this.newShardKey)[0];
        const zones = [
            {
                zone: zoneName,
                min: {[shardKeyField]: MinKey},
                max: {[shardKeyField]: MaxKey},
            },
        ];

        // Reshard with zones. numInitialChunks requires zones parameter to be passed directly.
        assert.commandWorked(
            connection.adminCommand({
                reshardCollection: ns,
                key: this.newShardKey,
                numInitialChunks: ReshardCollectionCommand.numInitialChunks,
                zones: zones,
            }),
        );
    }

    toString() {
        const type = isHashedShardKey(this.newShardKey) ? "hashed" : "range";
        return `ReshardCollectionCommand(${type})`;
    }

    getChangeEvents(watchMode) {
        // Reshard emits only reshardCollection event.
        // Index events are handled by separate CreateIndexCommand/DropIndexCommand.
        return [
            {
                operationType: "reshardCollection",
                ns: {db: this.dbName, coll: this.collName},
            },
        ];
    }
}

/**
 * Base rename collection command.
 * Subclasses define targetShouldExist and crossDatabase flags.
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * - At collection level: emits 'rename' then 'invalidate' (collection renamed away).
 * - If collection doesn't exist: no events are emitted.
 * - 'invalidate' is only emitted for collection-level change streams (watchMode === kCollection).
 * NOTE: MongoDB does NOT emit 'dropIndexes' when renaming collections.
 */
class RenameCommand extends Command {
    // Subclasses must set: this.targetShouldExist, this.crossDatabase.

    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
    }

    execute(connection) {
        const targetDb = this.crossDatabase ? `${this.dbName}_target` : this.dbName;
        const targetColl = `${this.collName}_renamed`;

        assert.commandWorked(
            connection.adminCommand({
                renameCollection: `${this.dbName}.${this.collName}`,
                to: `${targetDb}.${targetColl}`,
            }),
        );

        // Drop the renamed collection to ensure the target doesn't already exist for subsequent renames.
        assert.commandWorked(connection.getDB(targetDb).runCommand({drop: targetColl}));
    }

    toString() {
        const targetType = this.targetShouldExist ? "Existent" : "NonExistent";
        const dbType = this.crossDatabase ? "DifferentDb" : "SameDb";
        return `RenameTo${targetType}${dbType}Command`;
    }

    getChangeEvents(watchMode) {
        assert(watchMode !== undefined, "watchMode must be specified for getChangeEvents()");
        // If collection doesn't exist, no events are emitted.
        if (!this.collectionCtx.exists) {
            return [];
        }
        // NOTE: MongoDB does NOT emit 'dropIndexes' when renaming collections,
        // regardless of whether they were resharded or not. Verified via testing.
        const events = [
            {
                operationType: "rename",
                ns: {db: this.dbName, coll: this.collName},
            },
        ];
        if (watchMode === ChangeStreamWatchMode.kCollection) {
            events.push({operationType: "invalidate"});
        }
        // TODO SERVER-116455: For database/cluster level streams, the cleanup drop of the target
        // collection (executed at end of execute()) will also emit a drop event. This needs
        // verification with database-level change stream tests. If confirmed, add:
        //   if (watchMode === ChangeStreamWatchMode.kDb || watchMode === ChangeStreamWatchMode.kCluster) {
        //       const targetDb = this.crossDatabase ? `${this.dbName}_target` : this.dbName;
        //       events.push({operationType: "drop", ns: {db: targetDb, coll: `${this.collName}_renamed`}});
        //   }
        return events;
    }
}

// Concrete rename command classes.
class RenameToNonExistentSameDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
        this.targetShouldExist = false;
        this.crossDatabase = false;
    }
}

class RenameToExistentSameDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
        this.targetShouldExist = true;
        this.crossDatabase = false;
    }
}

class RenameToNonExistentDifferentDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
        this.targetShouldExist = false;
        this.crossDatabase = true;
    }
}

class RenameToExistentDifferentDbCommand extends RenameCommand {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
        this.targetShouldExist = true;
        this.crossDatabase = true;
    }
}

/**
 * Base class for move operations.
 * Handles shard selection and provides common move functionality.
 */
class MoveCommandBase extends Command {
    /**
     * Get the target shard for the move operation.
     * @param {Object} connection - The MongoDB connection.
     * @returns {string|null} The target shard ID, or null if no suitable shard exists.
     */
    _getTargetShard(connection) {
        throw new Error("_getTargetShard() method must be implemented by subclasses");
    }

    /**
     * Build the move command to execute.
     * @param {string} targetShardId - The target shard ID.
     * @returns {Object} The command object to send to adminCommand.
     */
    _buildMoveCommand(targetShardId) {
        throw new Error("_buildMoveCommand() method must be implemented by subclasses");
    }

    execute(connection) {
        // No-op: Move operations are not critical for state machine testing since.
        // sharding is not actually set up.
        // In a real implementation, this would execute:
        //   const targetShardId = this._getTargetShard(connection);
        //   const moveCommand = this._buildMoveCommand(targetShardId);
        //   assert.commandWorked(connection.adminCommand(moveCommand));
    }
}

/**
 * Move primary command.
 */
class MovePrimaryCommand extends MoveCommandBase {
    _getTargetShard(connection) {
        assert(this.shardSet && this.shardSet.length > 0, "Shard set must be provided for MovePrimaryCommand");
        assert.gt(this.shardSet.length, 1, "MovePrimaryCommand requires at least 2 shards");
        const dbInfo = assert.commandWorked(connection.adminCommand({getDatabaseVersion: this.dbName}));
        const otherShards = this.shardSet.filter((s) => s._id !== dbInfo.primary);
        assert.gt(
            otherShards.length,
            0,
            `MovePrimaryCommand requires at least one shard other than primary (${dbInfo.primary})`,
        );
        return otherShards[Random.randInt(otherShards.length)]._id;
    }

    _buildMoveCommand(targetShardId) {
        return {
            movePrimary: this.dbName,
            to: targetShardId,
        };
    }

    toString() {
        return "MovePrimaryCommand";
    }

    getChangeEvents(watchMode) {
        // movePrimary does not emit change stream events.
        return [];
    }
}

/**
 * Move collection command.
 */
class MoveCollectionCommand extends MoveCommandBase {
    _getTargetShard(connection) {
        assert(this.shardSet && this.shardSet.length > 0, "Shard set must be provided for MoveCollectionCommand");
        assert.gt(this.shardSet.length, 1, "MoveCollectionCommand requires at least 2 shards");
        return this.shardSet[Random.randInt(this.shardSet.length)]._id;
    }

    _buildMoveCommand(targetShardId) {
        return {
            moveCollection: `${this.dbName}.${this.collName}`,
            toShard: targetShardId,
        };
    }

    toString() {
        return "MoveCollectionCommand";
    }

    getChangeEvents(watchMode) {
        // moveCollection does not emit change stream events (it's a no-op in tests).
        return [];
    }
}

/**
 * Move chunk command.
 * TODO: SERVER-114858 - Improve chunk selection logic.
 */
class MoveChunkCommand extends MoveCommandBase {
    _getTargetShard(connection) {
        assert(this.shardSet && this.shardSet.length > 0, "Shard set must be provided for MoveChunkCommand");
        assert.gt(this.shardSet.length, 1, "MoveChunkCommand requires at least 2 shards");
        return this.shardSet[Random.randInt(this.shardSet.length)]._id;
    }

    _buildMoveCommand(targetShardId) {
        return {
            moveChunk: `${this.dbName}.${this.collName}`,
            find: {_id: MinKey},
            to: targetShardId,
        };
    }

    toString() {
        return "MoveChunkCommand";
    }

    getChangeEvents(watchMode) {
        // TODO SERVER-114858: moveChunk only emits change stream events in showSystemEvents mode,
        // which we don't use in our tests. For normal change streams, it's invisible.
        return [];
    }
}

// Export classes.
export {
    Command,
    InsertDocCommand,
    CreateDatabaseCommand,
    CreateIndexCommand,
    DropIndexCommand,
    ShardCollectionCommand,
    CreateUnsplittableCollectionCommand,
    CreateUntrackedCollectionCommand,
    DropCollectionCommand,
    DropDatabaseCommand,
    RenameToNonExistentSameDbCommand,
    RenameToExistentSameDbCommand,
    RenameToNonExistentDifferentDbCommand,
    RenameToExistentDifferentDbCommand,
    UnshardCollectionCommand,
    ReshardCollectionCommand,
    MovePrimaryCommand,
    MoveCollectionCommand,
    MoveChunkCommand,
    ShardingType,
    getShardKeySpec,
};
