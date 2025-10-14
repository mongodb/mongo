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

declare class Mongo {
    constructor(uri?: string, encryptedDBClientCallback?, options?: object);
    startSession(opts?): DriverSession;
    find(ns, query, fields, limit, skip, batchSize, options);
    insert(ns, obj);
    remove(ns, pattern);
    update(ns, query, obj, upsert);
    setSlaveOk(value);
    getSlaveOk();
    setSecondaryOk(value = true);
    getSecondaryOk();
    getDB(name: string): DB;
    getDBs(driverSession);

    adminCommand<Req extends Partial<Commands[keyof Commands]["req"]>>(
      command: GenericArguments & Req,
    ): GenericReplyFieldsAnd<GetResponseType<Req>>;

    adminCommand<ReqType extends keyof Commands>(
      command: GenericArguments & ReqType,
    ): GenericReplyFieldsAnd<Commands[ReqType]["res"]>;

    runCommand<Req extends Partial<Commands[keyof Commands]["req"]>>(
      dbName: str,
      command: GenericArguments & Req,
      options: object,
    ): MergeKeys<GenericReplyFields, GetResponseType<Req>>;

    runCommand<ReqType extends keyof Commands>(
      dbName: str,
      command: GenericArguments & ReqType,
      options: object,
    ): GenericReplyFieldsAnd<Commands[ReqType]["res"]>;

    getLogComponents(driverSession);
    setLogLevel();
    getDBNames();
    getCollection(ns);
    toString();
    tojson();
    setReadPref(mode, tagSet);
    getReadPrefMode();
    getReadPrefTagSet();
    getReadPref();
    setReadConcern(level);
    getReadConcern();
    setWriteConcern(wc);
    getWriteConcern();
    unsetWriteConcern();
    advanceClusterTime(newTime);
    resetClusterTime_forTesting();
    getClusterTime();
    startSession(options = {});
    isCausalConsistency();
    setCausalConsistency(causalConsistency = true);
    waitForClusterTime(maxRetries = 10);
    watch(pipeline, options);
}

declare function connect();
