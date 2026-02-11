// type declarations for mongo.js

type string = string;
type int = number;
type long = NumberLong;
type double = number;
type bool = boolean;
type TxnNumber = NumberLong;

type safeInt = int | long | double;
type safeInt64 = int | long | double;
type exactInt64 = int | long | double;
type safeDouble = int | long | double;

type safeBool = bool | int | long | double;

type optionalBool = boolean | undefined;

type bindata_generic = Uint8Array;
type bindata_function = Uint8Array;
type bindata_uuid = UUID;
type bindata_encrypt = Uint8Array;
type bindata_sensitive = Uint8Array;
type bindata_vector = Uint8Array;
type bindata_md5 = Uint8Array;

type uuid = bindata_uuid;

type objectid = string; // Usually represented as a hex string

type object = Record<string, unknown>;
type object_owned = Record<string, unknown>;

type array = unknown[];
type array_owned = unknown[];

type date = Date;

type millisEpoch = number; // ms since epoch
type unixEpoch = number; // s since epoch
type NamespaceString = string;

type Base64String = string;
type Base64UrlString = string;
type ConnectionString = string;
type FCVString = string;
type IDLAnyType = any;
type IDLAnyTypeOwned = any;
type TenantId = string;
type TenantIdHex = string;
type DatabaseName = string;

enum CollationCaseFirst {
  Upper = "upper",
  Lower = "lower",
  Off = "off",
}

enum CollationStrength {
  Primary = 1,
  Secondary = 2,
  Tertiary = 3,
  Quaternary = 4,
  Identical = 5,
}

enum CollationAlternate {
  NonIgnorable = "non-ignorable",
  Shifted = "shifted",
}

enum CollationMaxVariable {
  Punct = "punct",
  Space = "space",
}

// Structs

interface OkReply {
  ok: 1;
}

interface ErrorReply {
  ok: 0;
  code: number;
  codeName: string;
  errmsg: string;
  errorLabels?: string[];
}

interface SimpleCursorOptions {
  batchSize?: number; // safeInt64 with gte: 0
}

interface Collation {
  locale: string;
  caseLevel?: boolean; // default: false
  caseFirst?: CollationCaseFirst; // default: Off
  strength?: number; // default: CollationStrength.Tertiary (int between 0 and 5)
  numericOrdering?: boolean; // default: false
  alternate?: CollationAlternate; // default: NonIgnorable
  maxVariable?: CollationMaxVariable; // default: Punct
  normalization?: boolean; // default: false
  backwards?: boolean; // optionalBool
  version?: string;
}

interface Commands {}

type ConfigureFailPointModeObject = {
  /** n > 0. n: the probability that the fail point will fire.  0=never, 1=always. */
  activationProbability: number;

  /** n > 0. n: # of passes the fail point remains active. */
  times: number;

  /** n > 0. n: # of passes before the fail point activates and remains active. */
  skip: number;
};

interface Commands {
  /** Test-only command for modifying installed fail points.
   *
   * Requires the 'enableTestCommands' server parameter to be set. See docs/test_commands.md.
   */
  configureFailPoint: Command<
    {
      /** (command) Test-only command for modifying installed fail points.
       *
       * (Fields: `mode` `data`) */
      configureFailPoint: string;

      /** (field) The new mode to set for the failpoint. */
      mode?: "off" | "alwaysOn" | ConfigureFailPointModeObject;

      /** (field) optional arbitrary object to inject into the failpoint.
       * When activated, the FailPoint can read this data and it can be used to inform
       * the specific action taken by the code under test. */
      data?: object;
    },
    object
  >;
}

/**
 * Arguments accepted by all commands.
 */
type GenericArguments = {
  /** generic argument */ apiVersion?: string;
  /** generic argument */ apiStrict?: boolean;
  /** generic argument */ apiDeprecationErrors?: boolean;
  /** generic argument */ maxTimeMS?: number;
  /** generic argument */ readConcern?: ReadConcernIdl;
  /** generic argument */ writeConcern?: WriteConcernIdl;
  /** generic argument */ lsid?: LogicalSessionFromClient;
  /** generic argument */ clientOperationKey?: UUID;
  /** generic argument */ txnNumber?: TxnNumber;
  /** generic argument */ autocommit?: boolean;
  /** generic argument */ startTransaction?: boolean;
  /** generic argument */ comment?: any;
  /** generic argument */ $readPreference?: ReadPreference;
  /** generic argument */ $clusterTime?: ClusterTime;
  /** generic argument */ $audit?: AuditMetadata;
  /** generic argument */ $client?: object;
  /** generic argument */ $configServerState?: object;
  /** generic argument */ allowImplicitCollectionCreation?: boolean;
  /** generic argument */ $oplogQueryData?: any;
  /** generic argument */ $queryOptions?: object;
  /** generic argument */ $replData?: any;
  /** generic argument */ databaseVersion?: DatabaseVersion;
  /** generic argument */ help?: boolean;
  /** generic argument */ shardVersion?: ShardVersion;
  /** generic argument */ tracking_info?: object;
  /** generic argument */ coordinator?: boolean;
  /** generic argument */ maxTimeMSOpOnly?: number;
  /** generic argument */ usesDefaultMaxTimeMS?: boolean;
  /** generic argument */ $configTime?: Timestamp;
  /** generic argument */ $topologyTime?: Timestamp;
  /** generic argument */ txnRetryCounter?: TxnRetryCounter;
  /** generic argument */ versionContext?: VersionContext;
  /** generic argument */ mayBypassWriteBlocking?: boolean;
  /** generic argument */ expectPrefix?: boolean;
  /** generic argument */ requestGossipRoutingCache?: any[];
  /** generic argument */ startOrContinueTransaction?: boolean;
  /** generic argument */ rawData?: boolean | null;
};

/**
 * Fields that may appear in any command reply.
 */
type GenericReplyFields = {
  /** generic reply field */ $clusterTime?: ClusterTime;
  /** generic reply field */ operationTime?: LogicalTime;
  /** generic reply field */ $configServerState?: object;
  /** generic reply field */ $gleStats?: object;
  /** generic reply field */ lastCommittedOpTime?: OpTime;
  /** generic reply field */ readOnly?: boolean;
  /** generic reply field */ additionalParticipants?: object[];
  /** generic reply field */ $configTime?: Timestamp;
  /** generic reply field */ $replData?: object;
  /** generic reply field */ $topologyTime?: Timestamp;
  /** generic reply field */ $oplogQueryData?: object;
  /** generic reply field */ ok?: boolean;
  /** generic reply field */ routingCacheGossip?: GossipedRoutingCache[];
};

type Command<Req extends object = {}, Res = any> = {
  req: Req;
  res: Res;
};

// All keys that could be passed to a Req object.
type AllReqKeys = keyof {
  [K in keyof Commands as keyof Commands[K]["req"]]: any;
};

// Takes in a request object and deduces which command it came from and what its response type is.
type GetResponseType<T> = {
  [K in keyof Commands]: K extends keyof T ? Commands[K]["res"] : never;
}[keyof Commands];

class None {}

type MergeKeys<A, B> = Omit<A, keyof B> & B;
type GenericReplyFieldsAnd<T> = MergeKeys<GenericReplyFields, T>;

/**
 * MongoDB connection object.
 * Represents a connection to a MongoDB server or cluster.
 */
declare class Mongo {
    /**
     * Create a connection to MongoDB.
     * @param uri Connection string URI (e.g., "mongodb://localhost:27017" or "mongodb+srv://..."). Omit for default localhost.
     * @param encryptedDBClientCallback Optional callback for client-side field level encryption
     * @param options Connection options object
     *
     * To create a Mongo instance, use `connect(uri)` so the shell can choose the right connection type
     * (single node/replica set `Mongo` vs Multi-Router `Mongo`) based on the cluster topology.
     */
    constructor(uri?: string, encryptedDBClientCallback?, options?: object);

    /**
     * Start a new explicit client session.
     * Sessions enable causal consistency and are required for transactions.
     * @param opts Session options like causalConsistency, retryWrites, readPreference, readConcern, writeConcern
     * @returns New session object
     */
    startSession(opts?): DriverSession;

    /**
     * Low-level find operation on a collection namespace.
     * Most users should use db.collection.find() instead.
     * @param ns Full namespace (database.collection)
     * @param query Query filter document
     * @param fields Projection document
     * @param limit Number of documents to return
     * @param skip Number of documents to skip
     * @param batchSize Batch size for cursor
     * @param options Query options bitmask
     * @returns Query cursor
     */
    find(ns: string, query: object, fields: object, limit: number, skip: number, batchSize: number, options: number): DBQuery;

    /**
     * Low-level insert operation on a collection namespace.
     * Most users should use db.collection.insertOne/insertMany() instead.
     * @param ns Full namespace (database.collection)
     * @param obj Document or array of documents to insert
     * @returns Write result
     */
    insert(ns: string, obj: object | object[]): WriteResult;

    /**
     * Low-level remove operation on a collection namespace.
     * Most users should use db.collection.deleteOne/deleteMany() instead.
     * @param ns Full namespace (database.collection)
     * @param pattern Query filter for documents to remove
     * @returns Write result
     */
    remove(ns: string, pattern: object): WriteResult;

    /**
     * Low-level update operation on a collection namespace.
     * Most users should use db.collection.updateOne/updateMany() instead.
     * @param ns Full namespace (database.collection)
     * @param query Query filter for documents to update
     * @param obj Update document or replacement document
     * @param upsert If true, insert if no documents match
     * @returns Write result
     */
    update(ns: string, query: object, obj: object, upsert: boolean): WriteResult;

    /**
     * @deprecated Use setSecondaryOk() instead
     * Allow or disallow queries on secondary replica set members.
     * @param value True to allow secondary reads
     */
    setSlaveOk(value: boolean): void;

    /**
     * @deprecated Use getSecondaryOk() instead
     * Check if secondary reads are allowed.
     * @returns True if secondary reads are enabled
     */
    getSlaveOk(): boolean;

    /**
     * Allow queries to run on secondary replica set members.
     * Affects all databases on this connection.
     * @param value True to allow secondary reads (default: true)
     */
    setSecondaryOk(value?: boolean): void;

    /**
     * Check if queries are allowed on secondary replica set members.
     * @returns True if secondary reads are enabled
     */
    getSecondaryOk(): boolean;

    /**
     * Get a database object for the specified database name.
     * @param name Database name
     * @returns Database object
     */
    getDB(name: string): DB;

    /**
     * Get a list of all databases on the server.
     * @param driverSession Optional session to use for the operation
     * @returns Object with databases array and totalSize
     */
    getDBs(driverSession?): object;

    /**
     * Run a command against the admin database.
     * @param command Command object with command name and parameters
     * @returns Command result with ok field and command-specific fields
     */
    adminCommand<Req extends Partial<Commands[keyof Commands]["req"]>>(
      command: GenericArguments & Req,
    ): GenericReplyFieldsAnd<GetResponseType<Req>>;

    adminCommand<ReqType extends keyof Commands>(
      command: GenericArguments & ReqType,
    ): GenericReplyFieldsAnd<Commands[ReqType]["res"]>;

    /**
     * Run a command against a specific database.
     * Low-level method; most users should use db.runCommand() instead.
     * @param dbName Database name to run command against
     * @param command Command object with command name and parameters
     * @param options Additional options
     * @returns Command result
     */
    runCommand<Req extends Partial<Commands[keyof Commands]["req"]>>(
      dbName: string,
      command: GenericArguments & Req,
      options: object,
    ): MergeKeys<GenericReplyFields, GetResponseType<Req>>;

    runCommand<ReqType extends keyof Commands>(
      dbName: string,
      command: GenericArguments & ReqType,
      options: object,
    ): GenericReplyFieldsAnd<Commands[ReqType]["res"]>;

    /**
     * Get the current log verbosity levels for all components.
     * @param driverSession Optional session to use
     * @returns Object with verbosity levels by component
     */
    getLogComponents(driverSession?): object;

    /**
     * Set the log verbosity level for a component.
     * @param level Verbosity level (-1 to 5, where 0 is default)
     * @param component Component name (e.g., "query") or omit for global
     * @returns Command result
     */
    setLogLevel(level: number, component?: string): object;

    /**
     * Get the names of all databases on the server.
     * @returns Array of database name strings
     */
    getDBNames(): string[];

    /**
     * Get a collection object by full namespace.
     * @param ns Full namespace (database.collection)
     * @returns Collection object
     */
    getCollection(ns: string): DBCollection;

    /**
     * Get a string representation of the connection.
     * @returns Connection string or description
     */
    toString(): string;

    /**
     * Convert the connection to JSON string.
     * @returns JSON representation
     */
    tojson(): string;

    /**
     * Set the read preference for this connection.
     * Affects which replica set members are used for read operations.
     * @param mode Read preference mode ("primary", "primaryPreferred", "secondary", "secondaryPreferred", "nearest")
     * @param tagSet Optional array of tag sets for server selection
     */
    setReadPref(mode: string, tagSet?: object[]): void;

    /**
     * Get the current read preference mode.
     * @returns Read preference mode string
     */
    getReadPrefMode(): string;

    /**
     * Get the current read preference tag set.
     * @returns Array of tag set objects
     */
    getReadPrefTagSet(): object[];

    /**
     * Get the full read preference object.
     * @returns Read preference object with mode and tags
     */
    getReadPref(): object;

    /**
     * Set the read concern level for this connection.
     * @param level Read concern level ("local", "majority", "linearizable", "snapshot", or "available")
     */
    setReadConcern(level: string): void;

    /**
     * Get the current read concern level.
     * @returns Read concern level string or undefined if not set
     */
    getReadConcern(): string | undefined;

    /**
     * Set the write concern for this connection.
     * @param wc Write concern object with w, j, wtimeout fields
     */
    setWriteConcern(wc: object): void;

    /**
     * Get the current write concern.
     * @returns Write concern object or undefined if not set
     */
    getWriteConcern(): object | undefined;

    /**
     * Unset (remove) the write concern for this connection.
     */
    unsetWriteConcern(): void;

    /**
     * Manually advance the cluster time for this connection.
     * Used internally for causal consistency; most users don't need this.
     * @param newTime New cluster time object
     */
    advanceClusterTime(newTime: object): void;

    /**
     * Reset the cluster time to null (testing only).
     * @internal
     */
    resetClusterTime_forTesting(): void;

    /**
     * Get the current cluster time for this connection.
     * @returns Cluster time object or undefined
     */
    getClusterTime(): object | undefined;

    /**
     * Start a new explicit client session.
     * @param options Session options
     * @returns New session object
     */
    startSession(options?: object): DriverSession;

    /**
     * Check if causal consistency is enabled for this connection.
     * @returns True if causal consistency is enabled
     */
    isCausalConsistency(): boolean;

    /**
     * Enable or disable causal consistency for this connection.
     * @param causalConsistency True to enable (default: true)
     */
    setCausalConsistency(causalConsistency?: boolean): void;

    /**
     * Wait for the cluster time to advance (for testing).
     * @param maxRetries Maximum number of retry attempts (default: 10)
     */
    waitForClusterTime(maxRetries?: number): void;

    /**
     * Open a change stream to watch for changes across all databases.
     * @param pipeline Optional aggregation pipeline to filter change events
     * @param options Options like fullDocument, resumeAfter, startAtOperationTime
     * @returns Change stream cursor
     */
    watch(pipeline?: object[], options?: object): DBCommandCursor;
}

/**
 * Connect to MongoDB and return a connection object.
 *
 * This behaves like a connection factory:
 * - For a URI that targets a single mongod or a replica set, this returns a regular `Mongo`.
 * - For a URI that targets a mongos pool, this returns the multi-router variant.
 *
 * @param connectionString MongoDB connection string (e.g., "mongodb://localhost:27017" or "mongodb+srv://...")
 * @returns Mongo connection object
 * @example
 * let conn = connect("mongodb://localhost:27017").getMongo();
 * let db = conn.getDB("test");
 * 
 * or if you already have a mongo object and you with to reconnect with it:
 * let conn = connect(db.getMongo().uri).getMongo();
 */
declare function connect(connectionString: string): Mongo;
