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
 * INDEX EVENTS (createIndexes / dropIndexes)
 * ================================================================================
 * When MongoDB executes createIndexes or dropIndexes on a sharded collection, it is
 * one event per shard. We do not include these in expected events:
 * CreateIndexCommand and DropIndexCommand return [] from getChangeEvents();
 * ShardCollectionCommand omits the implicit createIndexes when sharding a new collection.
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
 * Get the DB primary shard.
 * @param {Object} connection - The MongoDB connection.
 * @returns {string} The primary shard ID.
 */
function getDbPrimary(connection, dbName) {
    const dbDoc = connection.getDB("config").databases.findOne({_id: dbName});
    assert(dbDoc, `${this}: database ${dbName} not in config.databases`);
    return dbDoc.primary;
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
     * Runs the command logic. Subclasses must override this.
     * Transient DDL errors are handled by the runCommand override
     * (implicitly_retry_on_migration_in_progress_fsm.js) loaded by the suite.
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

    /**
     * Serialize this command to a plain BSON-serializable object.
     * Used to pass commands across Thread boundaries.
     */
    toSpec() {
        return {type: this.constructor.name, ...this};
    }

    /** Registry populated by _registerAll() at module load time. */
    static _registry = {};

    /**
     * Reconstruct a Command instance from a spec produced by toSpec().
     */
    static fromSpec(spec) {
        const cls = Command._registry[spec.type];
        assert(cls, `Unknown command type: ${spec.type}. Did you forget to add it to _registerAll()?`);
        const cmd = Object.create(cls.prototype);
        Object.assign(cmd, spec);
        return cmd;
    }
}

/**
 * Insert document command.
 * The insertion behavior differs depending on whether the collection already exists.
 * By default inserts 2 documents with distinct shard-key values; pass a custom array
 * to override (e.g. for targeted shard-key placement tests).
 */
class InsertDocCommand extends Command {
    static numDocs = 2;

    /**
     * Insert documents in order, skipping any that already exist (from a
     * previous attempt when execute() retries after a transient error).
     */
    static insertDocuments(coll, documents) {
        for (const doc of documents) {
            const res = coll.insert(doc);
            if (res.hasWriteError() && res.getWriteError().code === ErrorCodes.DuplicateKey) {
                continue;
            }
            assert.commandWorked(res);
        }
    }

    constructor(dbName, collName, shardSet, collectionCtx, documents = null) {
        super(dbName, collName, shardSet, collectionCtx);
        if (documents) {
            this.documents = documents;
        } else {
            // Use ObjectId().str to get the unique hex string. The ObjectId BSON wrapper
            // itself becomes `{}` when passed through Thread boundaries, but the extracted
            // string survives serialization and is globally unique.
            this.documents = [];
            for (let i = 0; i < InsertDocCommand.numDocs; i++) {
                const id = new ObjectId().str;
                this.documents.push({
                    _id: id,
                    data: `data_${id}_${i}`,
                });
            }
        }
    }

    execute(connection) {
        const coll = connection.getDB(this.dbName).getCollection(this.collName);
        InsertDocCommand.insertDocuments(coll, this.documents);
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

        for (const doc of this.documents) {
            // Build documentKey: shard key fields first (if sharded), then _id.
            let documentKey = {_id: doc._id};
            if (this.collectionCtx.shardKeySpec) {
                for (const field of Object.keys(this.collectionCtx.shardKeySpec)) {
                    documentKey[field] = doc[field];
                }
            }
            events.push({
                operationType: "insert",
                ns: {db: this.dbName, coll: this.collName},
                fullDocument: doc,
                documentKey,
            });
        }
        return events;
    }
}

/**
 * Create database command.
 *
 * Does not pin a primary shard — the server picks one automatically.
 */
class CreateDatabaseCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx, primaryShard = null) {
        super(dbName, collName, shardSet, collectionCtx);
        this.primaryShard = primaryShard;
    }

    execute(connection) {
        const cmd = {enableSharding: this.dbName};
        if (this.primaryShard) {
            cmd.primaryShard = this.primaryShard;
        }
        assert.commandWorked(connection.adminCommand(cmd));
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
 * TODO SERVER-121676: Pins to the DB primary shard to avoid v2 shard-targeting
 * issues on resume. Once fixed, this can target a random shard again.
 */
class CreateUnsplittableCollectionCommand extends Command {
    constructor(dbName, collName, shardSet, collectionCtx, dataShard = null) {
        super(dbName, collName, shardSet, collectionCtx);
        this.dataShard = dataShard;
    }

    execute(connection) {
        const dataShard = this.dataShard ?? getDbPrimary(connection, this.dbName);
        const db = connection.getDB(this.dbName);
        assert.commandWorked(
            db.runCommand({
                createUnsplittableCollection: this.collName,
                dataShard,
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
        assert.commandWorked(connection.getDB(this.dbName).runCommand({drop: this.collName}));
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
        assert.commandWorked(connection.getDB(this.dbName).dropDatabase());
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
 * Used for command behavior (e.g. presplitHashedZones when sharding, toString).
 * Hashed keys cause immediate pre-split across shards; range keys start with one chunk.
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
 * Configure zone membership and key range to restrict a collection to specific shards.
 * Used by ShardCollectionCommand to pin initial placement.
 *
 * @param {Object} connection - MongoDB connection.
 * @param {string} ns - Full namespace (db.collection).
 * @param {Object} shardKeySpec - The shard key specification.
 * @param {Array} shardSet - Array of shard objects to restrict data to.
 */
function _configureZonesForShardSet(connection, ns, shardKeySpec, shardSet) {
    const zoneName = _getZoneName(ns);
    const shardKeyField = Object.keys(shardKeySpec)[0];

    for (const shard of shardSet) {
        assert.commandWorked(connection.adminCommand({addShardToZone: shard._id, zone: zoneName}));
    }

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
 * Update zone membership to match a new shard set. No-ops if membership already matches.
 * Used by ReshardCollectionCommand where the shard key changes (so updateZoneKeyRange
 * cannot be used -- it validates against the current key, not the new one).
 */
function _reconfigureZonesForShardSet(connection, ns, shardSet) {
    const zoneName = _getZoneName(ns);
    const newShardIds = new Set(shardSet.map((s) => s._id));

    const config = connection.getDB("config");
    const currentZoneShardIds = new Set(
        config.shards
            .find({tags: zoneName})
            .toArray()
            .map((s) => s._id),
    );

    if (currentZoneShardIds.size === newShardIds.size && [...newShardIds].every((id) => currentZoneShardIds.has(id))) {
        return;
    }

    for (const shardId of currentZoneShardIds) {
        if (!newShardIds.has(shardId)) {
            assert.commandWorked(connection.adminCommand({removeShardFromZone: shardId, zone: zoneName}));
        }
    }

    for (const shard of shardSet) {
        if (!currentZoneShardIds.has(shard._id)) {
            assert.commandWorked(connection.adminCommand({addShardToZone: shard._id, zone: zoneName}));
        }
    }
}

/**
 * Create index command.
 * Creates an index for the shard key (required before sharding or resharding).
 *
 * CHANGE STREAM EVENT BEHAVIOR (server): createIndexes emits one event per shard that has
 * data (1 for unsharded/range, shardSet.length for hashed). We do not include these in
 * expected events; getChangeEvents() returns [].
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
        // Do not emit events: per-shard createIndexes event count is not easily predictable.
        return [];
    }
}

/**
 * Drop index command.
 * Drops an old shard key index (cleanup after resharding to a new shard key).
 *
 * CHANGE STREAM EVENT BEHAVIOR (server): dropIndexes emits one event per shard that has
 * data (based on current shard key after reshard). We do not include these in expected
 * events; getChangeEvents() returns [].
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
        // Do not emit events: per-shard dropIndexes event count is not easily predictable.
        return [];
    }
}

/**
 * Shard existing collection command.
 * Sharding type (range vs hashed) is determined by the shardKeySpec in collectionCtx.
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * When sharding a non-existent collection, MongoDB implicitly: (1) creates the collection
 * (emits 'create'), (2) creates the shard key index (emits 'createIndexes'), (3) shards
 * (emits 'shardCollection'). We omit createIndexes from expected events.
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

        _configureZonesForShardSet(connection, ns, this.shardKey, this.shardSet);

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

        // When sharding a non-existent collection, MongoDB implicitly creates the collection
        // (emits 'create') then shards (emits 'shardCollection'). We omit createIndexes from expected events.
        if (!this.collectionCtx.exists) {
            events.push({
                operationType: "create",
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
 * TODO SERVER-121676: Pins to the DB primary shard to avoid v2 shard-targeting
 * issues on resume. Once fixed, this can target a random shard again.
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
        const dbDoc = connection.getDB("config").databases.findOne({_id: this.dbName});
        assert(dbDoc, `${this}: database ${this.dbName} not in config.databases`);
        const ns = `${this.dbName}.${this.collName}`;
        assert.commandWorked(
            connection.adminCommand({
                unshardCollection: ns,
                toShard: dbDoc.primary,
            }),
        );
    }

    toString() {
        return "UnshardCollectionCommand";
    }

    getChangeEvents(watchMode) {
        // unshardCollection internally uses reshard machinery to move data to a single shard,
        // so MongoDB emits a reshardCollection event (not an unshardCollection event).
        // Note: DropIndexCommand (old shard key cleanup) is run by the generator after this.
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
    constructor(
        dbName,
        collName,
        shardSet,
        collectionCtx,
        newShardKey,
        numInitialChunks = ReshardCollectionCommand.numInitialChunks,
    ) {
        super(dbName, collName, shardSet, collectionCtx);
        assert(newShardKey, "newShardKey must be provided to ReshardCollectionCommand");
        this.newShardKey = newShardKey;
        this.numInitialChunks = numInitialChunks;
    }

    execute(connection) {
        const ns = `${this.dbName}.${this.collName}`;

        // The prior ShardCollectionCommand may have configured zones for a different shard
        // set. Update zone membership to match the new shardSet before resharding.
        _reconfigureZonesForShardSet(connection, ns, this.shardSet);

        const zoneName = _getZoneName(ns);
        const shardKeyField = Object.keys(this.newShardKey)[0];
        assert.commandWorked(
            connection.adminCommand({
                reshardCollection: ns,
                key: this.newShardKey,
                numInitialChunks: this.numInitialChunks,
                zones: [
                    {
                        zone: zoneName,
                        min: {[shardKeyField]: MinKey},
                        max: {[shardKeyField]: MaxKey},
                    },
                ],
            }),
        );
    }

    toString() {
        const type = isHashedShardKey(this.newShardKey) ? "hashed" : "range";
        return `ReshardCollectionCommand(${type})`;
    }

    getChangeEvents(watchMode) {
        // Reshard emits only reshardCollection event.
        // Index events (createIndexes/dropIndexes) are handled by separate commands; we do not emit them.
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

        if (this.targetShouldExist) {
            assert.commandWorked(connection.getDB(targetDb).createCollection(targetColl));
        }

        assert.commandWorked(
            connection.adminCommand({
                renameCollection: `${this.dbName}.${this.collName}`,
                to: `${targetDb}.${targetColl}`,
                dropTarget: this.targetShouldExist,
            }),
        );

        // Drop the renamed collection to clean up for subsequent renames.
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
     * Get the shard to move from (the "source" shard for this operation).
     * For movePrimary: the current DB primary.
     * For moveCollection: the shard owning the collection (config.chunks for
     *   unsplittable, DB primary for untracked).
     * For moveChunk: not used (overrides execute() directly).
     */
    _getCurrentShard(connection) {
        throw new Error("_getCurrentShard() method must be implemented by subclasses");
    }

    /**
     * Build the move command to execute.
     * @param {string} targetShardId - The target shard ID.
     * @returns {Object} The command object to send to adminCommand.
     */
    _buildMoveCommand(targetShardId) {
        throw new Error("_buildMoveCommand() method must be implemented by subclasses");
    }

    /**
     * Look up the collection document and find the shard owning a chunk.
     * @param {Object} connection - The MongoDB connection.
     * @param {Object} [sort] - Optional sort for chunk lookup (default: no sort, returns any chunk).
     * @returns {string} The shard ID owning the chunk.
     */
    _getShardFromChunks(connection, sort = null) {
        const ns = `${this.dbName}.${this.collName}`;
        const configDb = connection.getDB("config");
        const collDoc = configDb.collections.findOne({_id: ns});
        assert(collDoc, `${this}: collection ${ns} not in config.collections`);
        const query = configDb.chunks.find({uuid: collDoc.uuid});
        const chunk = sort ? query.sort(sort).limit(1).next() : query.next();
        assert(chunk, `${this}: no chunks for ${ns}`);
        return chunk.shard;
    }

    /**
     * Pick a target shard different from the current shard.
     */
    _getTargetShard(connection) {
        assert(this.shardSet && this.shardSet.length > 1, `${this} requires a shard set with at least 2 shards`);
        const currentShard = this._getCurrentShard(connection);
        const otherShards = this.shardSet.filter((s) => s._id !== currentShard);
        assert.gt(otherShards.length, 0, `${this}: no other shard to move to (currently on ${currentShard})`);
        return otherShards[Random.randInt(otherShards.length)]._id;
    }

    execute(connection) {
        const targetShardId = this._getTargetShard(connection);
        const moveCommand = this._buildMoveCommand(targetShardId);
        assert.commandWorked(connection.adminCommand(moveCommand));
    }
}

/**
 * Move primary command.
 * TODO SERVER-122025: disabled in the FSM model — breaks cross-db rename.
 */
class MovePrimaryCommand extends MoveCommandBase {
    constructor(dbName, collName, shardSet, collectionCtx, targetShard = null) {
        super(dbName, collName, shardSet, collectionCtx);
        this.targetShard = targetShard;
    }

    _getTargetShard(connection) {
        return this.targetShard ?? super._getTargetShard(connection);
    }

    _getCurrentShard(connection) {
        return getDbPrimary(connection, this.dbName);
    }

    _buildMoveCommand(targetShardId) {
        assert(targetShardId, `MovePrimaryCommand: invalid target shard (got ${targetShardId})`);
        return {
            movePrimary: this.dbName,
            to: targetShardId,
        };
    }

    toString() {
        return "MovePrimaryCommand";
    }

    getChangeEvents(watchMode) {
        return [];
    }
}

/**
 * Move collection command.
 * TODO SERVER-121676: Currently disabled in the FSM model. Once shard-targeting on
 * resume is fixed, re-enable and allow random target shard selection.
 *
 * Resolves where the collection lives via _getCurrentShard: unsplittable → from
 * config.chunks, untracked → DB primary. The base _getTargetShard picks a different shard.
 *
 * CHANGE STREAM EVENT BEHAVIOR:
 * ============================
 * moveCollection uses reshard machinery internally and always emits a 'reshardCollection'
 * event.
 */
class MoveCollectionCommand extends MoveCommandBase {
    _getCurrentShard(connection) {
        if (this.collectionCtx.isUnsplittable) {
            return this._getShardFromChunks(connection);
        }
        return getDbPrimary(connection, this.dbName);
    }

    _buildMoveCommand(targetShardId) {
        assert(targetShardId, `MoveCollectionCommand: invalid target shard (got ${targetShardId})`);
        return {
            moveCollection: `${this.dbName}.${this.collName}`,
            toShard: targetShardId,
        };
    }

    toString() {
        return "MoveCollectionCommand";
    }

    getChangeEvents(watchMode) {
        // moveCollection always emits reshardCollection.
        return [
            {
                operationType: "reshardCollection",
                ns: {db: this.dbName, coll: this.collName},
            },
        ];
    }
}

/**
 * Move chunk command.
 * Inserts documents, splits into multiple chunks, then drains all chunks off the
 * donor shard (round-robin across other shards) so the donor ends up with zero
 * chunks. Exercises the "shard has no data for this collection" scenario, which
 * is important for change stream shard targeting.
 *
 * Interleaved inserts happen between chunk moves to verify change streams remain
 * functional during migrations. All inserted documents (initial + interleaved)
 * are reported by getChangeEvents().
 */
class MoveChunkCommand extends MoveCommandBase {
    constructor(dbName, collName, shardSet, collectionCtx) {
        super(dbName, collName, shardSet, collectionCtx);
        // Always insert enough documents for proper chunk distribution,
        // even when the collection already has data — prior inserts may
        // have fewer docs than shardSet.length requires for splitting.
        const numDocs = Math.max(shardSet.length + 1, InsertDocCommand.numDocs);
        this.documentsToInsert = [];
        for (let i = 0; i < numDocs; i++) {
            const id = new ObjectId().str;
            this.documentsToInsert.push({_id: id, data: `data_${id}_${i}`});
        }

        this.interleavedDocuments = [];
        for (let i = 0; i < InsertDocCommand.numDocs; i++) {
            const id = new ObjectId().str;
            this.interleavedDocuments.push({_id: id, data: `interleave_${id}_${i}`});
        }
    }

    /**
     * Identify the donor shard (the shard with the most chunks).
     * With balancer off, all chunks are on the primary shard for range sharding.
     * For hashed sharding with presplitHashedZones, chunks may be distributed
     * across shards at creation time.
     */
    _getDonorShardId(connection) {
        assert(this.shardSet && this.shardSet.length > 1, `${this} requires at least 2 shards`);
        const ns = `${this.dbName}.${this.collName}`;
        const configDb = connection.getDB("config");
        const collDoc = configDb.collections.findOne({_id: ns});
        assert(collDoc, `${this}: collection ${ns} not in config.collections`);
        const chunks = configDb.chunks.find({uuid: collDoc.uuid}).sort({"min": 1}).toArray();
        assert.gt(chunks.length, 0, `${this}: no chunks for ${ns}`);

        const chunksByShard = new Map();
        for (const c of chunks) {
            if (!chunksByShard.has(c.shard)) chunksByShard.set(c.shard, 0);
            chunksByShard.set(c.shard, chunksByShard.get(c.shard) + 1);
        }

        let donorShardId = null;
        let maxChunkCount = -1;
        for (const [shardId, chunkCount] of chunksByShard.entries()) {
            if (chunkCount > maxChunkCount) {
                maxChunkCount = chunkCount;
                donorShardId = shardId;
            }
        }
        assert(donorShardId !== null, `${this}: no shards have chunks for ${ns}`);
        return donorShardId;
    }

    /**
     * Split into multiple chunks if the collection doesn't have enough.
     * Exits early when chunkCount >= shardSet.length (same check for range and hashed).
     * Hashed collections with presplitHashedZones typically already have enough chunks
     * at creation time, so this is usually a no-op for them.
     */
    _ensureMultipleChunks(connection) {
        const ns = `${this.dbName}.${this.collName}`;
        const configDb = connection.getDB("config");
        const collDoc = configDb.collections.findOne({_id: ns});
        assert(collDoc, `${this}: collection ${ns} not in config.collections`);

        const chunkCount = configDb.chunks.countDocuments({uuid: collDoc.uuid});
        if (chunkCount >= this.shardSet.length) {
            return;
        }

        const shardKeyField = Object.keys(collDoc.key)[0];
        if (isHashedShardKey(collDoc.key)) {
            this._splitHashedChunks(connection, ns, shardKeyField, configDb, collDoc);
        } else {
            this._splitRangeChunks(connection, ns, shardKeyField);
        }
    }

    /**
     * Split a hashed collection using `find`-based splits. MongoDB hashes
     * the document value, locates the containing chunk, and splits at its
     * median. This avoids the problem of arbitrary NumberLong split points
     * (e.g. 1000, 2000) clustering near zero in the int64 hash range and
     * leaving the middle chunk nearly empty.
     */
    _splitHashedChunks(connection, ns, shardKeyField, configDb, collDoc) {
        const coll = connection.getDB(this.dbName).getCollection(this.collName);
        const docs = coll.find({}, {[shardKeyField]: 1, _id: 0}).toArray();

        for (const doc of docs) {
            if (configDb.chunks.countDocuments({uuid: collDoc.uuid}) >= this.shardSet.length) {
                break;
            }
            assert.commandWorked(
                connection.adminCommand({
                    split: ns,
                    find: {[shardKeyField]: doc[shardKeyField]},
                }),
            );
        }
    }

    /**
     * Split a range collection at actual data values so every chunk contains
     * documents. Numeric split points don't work because data values are
     * strings and BSON orders Numbers < Strings, which puts all data into
     * a single chunk.
     */
    _splitRangeChunks(connection, ns, shardKeyField) {
        const coll = connection.getDB(this.dbName).getCollection(this.collName);
        const values = coll
            .find({}, {[shardKeyField]: 1, _id: 0})
            .sort({[shardKeyField]: 1})
            .toArray()
            .map((doc) => doc[shardKeyField]);

        assert.gte(
            values.length,
            this.shardSet.length,
            `${this}: need >= ${this.shardSet.length} docs for splitting, got ${values.length}`,
        );

        for (let i = 1; i < this.shardSet.length; i++) {
            const idx = Math.floor((i * values.length) / this.shardSet.length);
            assert.commandWorked(
                connection.adminCommand({
                    split: ns,
                    middle: {[shardKeyField]: values[idx]},
                }),
            );
        }
    }

    _buildMoveChunkCmd(ns, chunk, targetShardId) {
        // Use `bounds` (not `find`) to identify the chunk by its exact
        // [min, max) range. `find` doesn't work for hashed shard keys
        // because it re-hashes the value, resolving to the wrong chunk.
        return {
            moveChunk: ns,
            bounds: [chunk.min, chunk.max],
            to: targetShardId,
            _waitForDelete: true,
        };
    }

    execute(connection) {
        const coll = connection.getDB(this.dbName).getCollection(this.collName);
        InsertDocCommand.insertDocuments(coll, this.documentsToInsert);

        this._ensureMultipleChunks(connection);

        const ns = `${this.dbName}.${this.collName}`;
        const configDb = connection.getDB("config");
        const collDoc = configDb.collections.findOne({_id: ns});
        assert(collDoc, `${this}: collection ${ns} not in config.collections`);

        const donorShardId = this._getDonorShardId(connection);
        const otherShardIds = this.shardSet.filter((s) => s._id !== donorShardId).map((s) => s._id);
        assert.gt(otherShardIds.length, 0, `${this}: no recipient shards to drain donor ${donorShardId}`);

        this._drainChunksFromDonor(connection, ns, configDb, collDoc, donorShardId, otherShardIds);
    }

    /**
     * Move all chunks off the donor shard round-robin across recipients.
     * Inserts interleaved documents after the first move, then re-queries
     * the donor to catch any chunks that appeared during the insert.
     */
    _drainChunksFromDonor(connection, ns, configDb, collDoc, donorShardId, otherShardIds) {
        let moved = 0;
        let donorChunks = configDb.chunks.find({uuid: collDoc.uuid, shard: donorShardId}).sort({min: 1}).toArray();

        while (donorChunks.length > 0) {
            for (let i = 0; i < donorChunks.length; i++) {
                const recipient = otherShardIds[(moved + i) % otherShardIds.length];
                assert.commandWorked(connection.adminCommand(this._buildMoveChunkCmd(ns, donorChunks[i], recipient)));

                if (moved + i === 0 && this.interleavedDocuments.length > 0) {
                    const coll = connection.getDB(this.dbName).getCollection(this.collName);
                    InsertDocCommand.insertDocuments(coll, this.interleavedDocuments);
                }
            }
            moved += donorChunks.length;

            // Re-query to catch any chunks that appeared on the donor
            // (e.g. from auto-splitting triggered by the interleaved insert).
            donorChunks = configDb.chunks.find({uuid: collDoc.uuid, shard: donorShardId}).sort({min: 1}).toArray();
        }

        assert.eq(
            configDb.chunks.countDocuments({uuid: collDoc.uuid, shard: donorShardId}),
            0,
            `${this}: donor ${donorShardId} still has chunks after drain`,
        );
    }

    toString() {
        return "MoveChunkCommand";
    }

    getChangeEvents(watchMode) {
        return [...this.documentsToInsert, ...this.interleavedDocuments].map((doc) => ({
            operationType: "insert",
            ns: {db: this.dbName, coll: this.collName},
            fullDocument: doc,
        }));
    }
}

function _registerAll(...classes) {
    for (const cls of classes) {
        Command._registry[cls.name] = cls;
    }
}

_registerAll(
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
);

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
