// type declarations for session.js

/**
 * Options for configuring a MongoDB session.
 */
declare class SessionOptions {
    constructor(rawOptions?: object);

    /**
     * Get the read preference for this session.
     * @returns Read preference object or undefined
     */
    getReadPreference(): object | undefined;

    /**
     * Set the read preference for this session.
     * @param readPreference Read preference object
     */
    setReadPreference(readPreference: object): void;

    /**
     * Get the read concern for this session.
     * @returns Read concern object or undefined
     */
    getReadConcern(): object | undefined;

    /**
     * Set the read concern for this session.
     * @param readConcern Read concern object
     */
    setReadConcern(readConcern: object): void;

    /**
     * Get the write concern for this session.
     * @returns Write concern object or undefined
     */
    getWriteConcern(): object | undefined;

    /**
     * Set the write concern for this session.
     * @param writeConcern Write concern object
     */
    setWriteConcern(writeConcern: object): void;

    /**
     * Check if causal consistency is enabled.
     * @returns True if causal consistency is enabled
     */
    isCausalConsistency(): boolean;

    /**
     * Check if retryable writes should be used.
     * @returns True if retryable writes are enabled
     */
    shouldRetryWrites(): boolean;

    /**
     * Get the raw options object.
     * @returns Raw options
     */
    getRawOpts(): object;

    /**
     * Shell print representation.
     * @returns String for shell printing
     */
    shellPrint(): string;

    /**
     * Convert to JSON string.
     * @returns JSON representation
     */
    tojson(...args: any[]): string;

    /**
     * Convert to string.
     * @returns String representation
     */
    toString(): string;
}

/**
 * Session-aware client wrapper that handles read/write concerns and preferences.
 */
declare class SessionAwareClient {
    /**
     * Get the read preference for the session.
     * @param driverSession Driver session
     * @returns Read preference object or null
     */
    getReadPreference(driverSession: DriverSession): object | null;

    /**
     * Get the read concern for the session.
     * @param driverSession Driver session
     * @returns Read concern object or null
     */
    getReadConcern(driverSession: DriverSession): object | null;

    /**
     * Get the write concern for the session.
     * @param driverSession Driver session
     * @returns Write concern object or undefined
     */
    getWriteConcern(driverSession: DriverSession): object | undefined;

    /**
     * Check if read concern can be used for the command.
     * @param driverSession Driver session
     * @param cmdObj Command object
     * @returns True if read concern can be used
     */
    canUseReadConcern(driverSession: DriverSession, cmdObj: object): boolean;

    /**
     * Prepare a command request with session information.
     * @param driverSession Driver session
     * @param cmdObj Command object
     * @returns Modified command object
     */
    prepareCommandRequest(driverSession: DriverSession, cmdObj: object): object;

    /**
     * Run a command through the session-aware client.
     * @param driverSession Driver session
     * @param dbName Database name
     * @param cmdObj Command object
     * @param options Command options
     * @returns Command result
     */
    runCommand(driverSession: DriverSession, dbName: string, cmdObj: object, options: any): any;
}

/**
 * MongoDB driver session for handling client sessions and transactions.
 */
declare class DriverSession {
    /**
     * Get the MongoDB client for this session.
     * @returns Mongo connection object
     */
    getClient(): Mongo;

    /**
     * Get the session-aware client wrapper.
     * @returns Session-aware client
     */
    _getSessionAwareClient(): SessionAwareClient;

    /**
     * Get the session options.
     * @returns Session options
     */
    getOptions(): SessionOptions;

    /**
     * Get the session ID.
     * @returns Session ID object or null
     */
    getSessionId(): object | null;

    /**
     * Get the transaction number (for testing).
     * @returns Transaction number
     */
    getTxnNumber_forTesting(): NumberLong;

    /**
     * Get the transaction write concern (for testing).
     * @returns Write concern object
     */
    getTxnWriteConcern_forTesting(): object;

    /**
     * Set the transaction number (for testing).
     * @param newTxnNumber New transaction number
     */
    setTxnNumber_forTesting(newTxnNumber: NumberLong): void;

    /**
     * Get the operation time for this session.
     * @returns Operation time timestamp or undefined
     */
    getOperationTime(): Timestamp | undefined;

    /**
     * Advance the operation time for this session.
     * @param operationTime New operation time
     */
    advanceOperationTime(operationTime: Timestamp): void;

    /**
     * Reset the operation time (for testing).
     */
    resetOperationTime_forTesting(): void;

    /**
     * Get the cluster time for this session.
     * @returns Cluster time object or undefined
     */
    getClusterTime(): object | undefined;

    /**
     * Advance the cluster time for this session.
     * @param clusterTime New cluster time
     */
    advanceClusterTime(clusterTime: object): void;

    /**
     * Reset the cluster time (for testing).
     */
    resetClusterTime_forTesting(): void;

    /**
     * Get a database object for this session.
     * @param dbName Database name
     * @returns Database object with session attached
     */
    getDatabase(dbName: string): DB;

    /**
     * Check if the session has ended.
     * @returns True if session has ended
     */
    hasEnded(): boolean;

    /**
     * End this session and release resources.
     */
    endSession(): void;

    /**
     * Shell print representation.
     * @returns String for shell printing
     */
    shellPrint(): string;

    /**
     * Convert to JSON string.
     * @returns JSON representation
     */
    tojson(...args: any[]): string;

    /**
     * Convert to string.
     * @returns String representation
     */
    toString(): string;

    /**
     * Start a multi-document transaction.
     * @param txnOptsObj Transaction options (readConcern, writeConcern, readPreference, maxCommitTimeMS)
     */
    startTransaction(txnOptsObj?: object): void;

    /**
     * Start a transaction (for testing).
     * @param txnOptsObj Transaction options
     * @param startNewTxnNumber Whether to start a new transaction number
     */
    startTransaction_forTesting(txnOptsObj?: object, startNewTxnNumber?: boolean): void;

    /**
     * Commit the active transaction.
     */
    commitTransaction(): void;

    /**
     * Abort the active transaction.
     */
    abortTransaction(): void;

    /**
     * Commit the transaction (for testing).
     */
    commitTransaction_forTesting(): void;

    /**
     * Abort the transaction (for testing).
     */
    abortTransaction_forTesting(): void;

    /**
     * Process a command response (for testing).
     * @param res Command response object
     */
    processCommandResponse_forTesting(res: object): void;
}

/**
 * Delegating driver session that wraps another session.
 */
declare class DelegatingDriverSession {
    /**
     * Get the MongoDB client.
     * @returns Mongo connection object
     */
    getClient(): Mongo;

    /**
     * Get the session-aware client wrapper.
     * @returns Session-aware client
     */
    _getSessionAwareClient(): SessionAwareClient;

    /**
     * Get a database object for this session.
     * @param dbName Database name
     * @returns Database object with session attached
     */
    getDatabase(dbName: string): DB;
}
