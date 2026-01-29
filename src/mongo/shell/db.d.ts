// type declarations for db.js, which extends objects/methods implemented in cpp.

declare class DB {

    constructor(mongo: Mongo, name: string);

    /**
     * Get the Mongo connection object for this database.
     * @returns The Mongo connection object
     */
    getMongo(): Mongo

    /**
     * Get the name of this database.
     * @returns The database name
     */
    getName(): string

    /**
     * Dynamic property to produce a DBCollection.
     * Access collections using dot notation or bracket notation.
     *
     * @example
     * let coll1 = db[jsTestName()];
     * let coll2 = db.myCollection;
     */
    [collectionIndex: string]: DBCollection

    /**
     * Rotate server certificates, CRLs, and CA files.
     * @param message Optional message to log when certificates are rotated
     * @returns Command result object
     */
    rotateCertificates(message?: string): object

    /**
     * Get another database from the same connection.
     * @param name Name of the database to retrieve
     * @returns The requested database object
     */
    getSiblingDB(name: string): DB

    /**
     * Get statistics about the database.
     * @param opt Scale factor (number) or options object. Scale converts sizes (1024 for KB, 1024*1024 for MB, etc.)
     * @returns Database statistics object
     */
    stats(opt?: number | object): object

    /**
     * Get a collection from this database.
     * @param name Name of the collection
     * @returns The collection object
     */
    getCollection(name: string): DBCollection

    /**
     * Get help text for a specific database command.
     * @param name Command name
     * @returns Help text for the command
     */
    commandHelp(name: string): string

    /**
     * Run a database command with read preference applied.
     * @param obj Command object or command name string
     * @param extra Additional command options (if obj is a string)
     * @param queryOptions Query options bitmask
     * @returns Command result object
     */
    runReadCommand(obj: object | string, extra?: object, queryOptions?: number): object

    /**
     * Run a database command.
     * @param command Command object with command name and parameters
     * @param extra Additional options to merge into the command
     * @param queryOptions Query options bitmask
     * @returns Command result object with ok field and command-specific fields
     */
    runCommand<Req extends Partial<Commands[keyof Commands]["req"]>>(
      command: GenericArguments & Req,
      extra?: object,
      queryOptions?: number,
    ): GenericReplyFieldsAnd<GetResponseType<Req>>

    /**
     * Run a command against the admin database (switches to admin db).
     * @param obj Command object or command name string
     * @param extra Additional command options (if obj is a string)
     * @returns Command result object
     */
    adminCommand(obj: object | string, extra?: object): object

    /**
     * Perform a collectionless aggregation on the database.
     * Runs aggregation without targeting a specific collection.
     * @param pipeline Array of aggregation pipeline stages
     * @param aggregateOptions Options like cursor batch size, maxTimeMS, readConcern, etc.
     * @returns Aggregation result cursor
     */
    aggregate(pipeline: object[], aggregateOptions?: object): object

    /**
     * Create a new collection in the database.
     * @param name Name of the collection to create
     * @param opt Options like capped, size, max, storageEngine, validator, etc.
     * @returns Command result object with ok field
     */
    createCollection(name: string, opt?: object): object

    /**
     * Create a view based on an aggregation pipeline.
     * @param name Name of the view to create
     * @param viewOn Name of the source collection or view
     * @param pipeline Aggregation pipeline array (or single stage object)
     * @param opt Options like collation
     * @returns Command result object with ok field
     */
    createView(name: string, viewOn: string, pipeline?: object | object[], opt?: object): object

    /**
     * @deprecated Use getProfilingStatus() instead
     * Get the current profiling level.
     * @returns Profiling level: 0 (off), 1 (slow operations), 2 (all operations), or null on error
     */
    getProfilingLevel(): number | null

    /**
     * Get the profiling status including level and slowms threshold.
     * @returns Object with 'was' (level), 'slowms', and other profiling settings
     */
    getProfilingStatus(): object

    /**
     * Drop (delete) the entire database.
     * @param writeConcern Write concern for the operation
     * @returns Command result object with ok field
     */
    dropDatabase(writeConcern?: object): object

    /**
     * Shutdown the MongoDB server (must be run against admin database).
     * @param opts Shutdown options
     * @param opts.force Force shutdown even if not connected to a secondary
     * @param opts.timeoutSecs Time to wait for secondaries to catch up before forcing
     */
    shutdownServer(opts?: { force?: boolean, timeoutSecs?: number }): void
    /**
     * Print help text for database methods.
     */
    help(): void

    /**
     * Print statistics for all collections in the database.
     * @param scale Scale factor for sizes (e.g., 1024 for KB)
     */
    printCollectionStats(scale?: number): void

    /**
     * Set the database profiling level and options.
     * @param level Profiling level: 0 (off), 1 (slow operations only), 2 (all operations)
     * @param options Slowms threshold (number) or options object with slowms and other settings
     * @returns Command result object
     */
    setProfilingLevel(level: number, options?: number | object): object

    /**
     * @deprecated Server-side JavaScript execution is deprecated
     * Execute a JavaScript function on the server.
     * @param jsfunction JavaScript function to execute server-side
     * @param args Arguments to pass to the function
     * @returns Result of the function execution
     */
    eval(jsfunction: Function, ...args: any[]): any

    /**
     * @deprecated
     * Group documents and evaluate an aggregation function.
     * @param parmsObj Parameters object with key, reduce, initial, etc.
     * @returns Aggregation results
     */
    groupeval(parmsObj: object): any

    /**
     * Force an error for testing purposes.
     */
    forceError(): void

    /**
     * Get names of all collections in the database.
     * @returns Array of collection name strings
     */
    getCollectionNames(): string[]

    /**
     * Convert database to JSON string representation.
     * @returns The database name
     */
    tojson(): string

    /**
     * Convert database to string representation.
     * @returns The database name
     */
    toString(): string

    /**
     * @deprecated Use hello() instead
     * Check if this node is the primary in a replica set.
     * @returns isMaster command result
     */
    isMaster(): object

    /**
     * Check replica set status and get server topology information.
     * @returns hello command result with topology and replica set information
     */
    hello(): object

    /**
     * Get information about currently executing operations.
     * @param arg Pass true to include all operations, or an object to filter results
     * @returns Object with 'inprog' array of current operations
     */
    currentOp(arg?: boolean | object): object

    /**
     * Get a cursor for currently executing operations.
     * @param arg Pass true to include all operations, or an object to filter results
     * @returns Cursor over current operations
     */
    currentOpCursor(arg?: boolean | object): DBCommandCursor

    /**
     * Kill a running operation.
     * @param op Operation ID (number) or connection ID (string) to kill
     * @returns Command result object
     */
    killOp(op: number | string): object
    /**
     * Get replication information for this database.
     * @returns Object with replication lag and oplog information
     */
    getReplicationInfo(): object

    /**
     * Print formatted replication status information to the console.
     */
    printReplicationInfo(): void

    /**
     * @deprecated Use printSecondaryReplicationInfo() instead
     * Print replication information for secondary nodes.
     */
    printSlaveReplicationInfo(): void

    /**
     * Print replication information for secondary (non-primary) nodes.
     */
    printSecondaryReplicationInfo(): void

    /**
     * Get detailed server build information.
     * @returns Object with version, gitVersion, modules, allocator, javascriptEngine, etc.
     */
    serverBuildInfo(): object

    /**
     * Get comprehensive server status including metrics and statistics.
     * @param options Optional filter for specific sections (e.g., {repl: 1, metrics: 0})
     * @returns Large object with server metrics, connections, operations, locks, etc.
     */
    serverStatus(options?: object): object

    /**
     * Get information about the host system running MongoDB.
     * @returns Object with system info, OS, CPUs, memory, etc.
     */
    hostInfo(): object

    /**
     * Get the command line options used to start the server.
     * @returns Object with parsed and argv arrays
     */
    serverCmdLineOpts(): object

    /**
     * Get the MongoDB server version string.
     * @returns Version string (e.g., "7.0.0")
     */
    version(): string

    /**
     * Get the server process architecture.
     * @returns 32 or 64 (bits)
     */
    serverBits(): number

    /**
     * List all available database commands with descriptions.
     * @returns Object mapping command names to their descriptions
     */
    listCommands(): object

    /**
     * Print sharding configuration and chunk distribution.
     * @param verbose If true, show detailed chunk information
     */
    printShardingStatus(verbose?: boolean): void

    /**
     * Lock the server and flush all data to disk for backup purposes.
     * @returns Command result object
     */
    fsyncLock(): object

    /**
     * Unlock the server after an fsyncLock operation.
     * @returns Command result object
     */
    fsyncUnlock(): object

    /**
     * @deprecated Use setSecondaryOk() instead
     * Allow or disallow queries on secondary replica set members.
     * @param value True to allow secondary reads
     */
    setSlaveOk(value?: boolean): void

    /**
     * @deprecated Use getSecondaryOk() instead
     * Check if secondary reads are allowed.
     * @returns True if secondary reads are enabled
     */
    getSlaveOk(): boolean

    /**
     * Allow queries to run on secondary replica set members.
     * @param value True to allow secondary reads (default: true)
     */
    setSecondaryOk(value?: boolean): void

    /**
     * Check if queries are allowed on secondary replica set members.
     * @returns True if secondary reads are enabled
     */
    getSecondaryOk(): boolean

    /**
     * Get the query options bitmask for this database.
     * @returns Options bitmask (includes slaveOk, etc.)
     */
    getQueryOptions(): number

    /**
     * Load and execute all JavaScript code stored in the db.system.js collection.
     */
    loadServerScripts(): void
    /**
     * Create a new user in the database.
     * @param userObj User document with user, pwd, roles, and optional customData, authenticationRestrictions
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    createUser(userObj: object, writeConcern?: object): object

    /**
     * Update an existing user's properties (roles, password, customData, etc.).
     * @param name Username to update
     * @param updateObject Update specification with fields to change
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    updateUser(name: string, updateObject: object, writeConcern?: object): object

    /**
     * Change a user's password.
     * @param username Username whose password to change
     * @param password New password
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    changeUserPassword(username: string, password: string, writeConcern?: object): object

    /**
     * Log out the currently authenticated user from this database.
     * @returns Command result object
     */
    logout(): object

    /**
     * @deprecated Use dropUser() instead
     * Remove a user from the database.
     * @param username Username to remove
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    removeUser(username: string, writeConcern?: object): object

    /**
     * Remove a user from the database.
     * @param username Username to drop
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    dropUser(username: string, writeConcern?: object): object

    /**
     * Remove all users from the database.
     * @param writeConcern Write concern for the operation
     * @returns Command result object with count of users removed
     */
    dropAllUsers(writeConcern?: object): object

    /**
     * Authenticate a user against this database.
     * @param username Username to authenticate (or omit to prompt)
     * @param password Password (or omit to prompt)
     * @returns Authentication result object
     */
    auth(username?: string, password?: string): object

    /**
     * Grant additional roles to a user.
     * @param username Username to grant roles to
     * @param roles Array of role names (strings) or role documents with role and db
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    grantRolesToUser(username: string, roles: string[] | object[], writeConcern?: object): object

    /**
     * Revoke roles from a user.
     * @param username Username to revoke roles from
     * @param roles Array of role names (strings) or role documents with role and db
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    revokeRolesFromUser(username: string, roles: string[] | object[], writeConcern?: object): object

    /**
     * Get information about a specific user.
     * @param username Username to retrieve
     * @param args Optional arguments like showCredentials, showPrivileges, showAuthenticationRestrictions
     * @returns User document or null if not found
     */
    getUser(username: string, args?: object): object | null

    /**
     * Get information about all users in the database.
     * @param args Optional filter and options
     * @returns Array of user documents
     */
    getUsers(args?: object): object[]

    /**
     * Create a new role in the database.
     * @param roleObj Role document with role, privileges, roles
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    createRole(roleObj: object, writeConcern?: object): object

    /**
     * Update an existing role's properties.
     * @param name Role name to update
     * @param updateObject Update specification
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    updateRole(name: string, updateObject: object, writeConcern?: object): object

    /**
     * Drop a role from the database.
     * @param name Role name to drop
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    dropRole(name: string, writeConcern?: object): object

    /**
     * Drop all user-defined roles from the database.
     * @param writeConcern Write concern for the operation
     * @returns Command result object with count of roles dropped
     */
    dropAllRoles(writeConcern?: object): object

    /**
     * Grant roles to an existing role (role inheritance).
     * @param rolename Role to grant roles to
     * @param roles Array of role names or role documents
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    grantRolesToRole(rolename: string, roles: string[] | object[], writeConcern?: object): object

    /**
     * Revoke roles from an existing role.
     * @param rolename Role to revoke roles from
     * @param roles Array of role names or role documents
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    revokeRolesFromRole(rolename: string, roles: string[] | object[], writeConcern?: object): object

    /**
     * Grant privileges to a role.
     * @param rolename Role to grant privileges to
     * @param privileges Array of privilege documents with resource and actions
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    grantPrivilegesToRole(rolename: string, privileges: object[], writeConcern?: object): object

    /**
     * Revoke privileges from a role.
     * @param rolename Role to revoke privileges from
     * @param privileges Array of privilege documents with resource and actions
     * @param writeConcern Write concern for the operation
     * @returns Command result object
     */
    revokePrivilegesFromRole(rolename: string, privileges: object[], writeConcern?: object): object

    /**
     * Get information about a specific role.
     * @param rolename Role name to retrieve
     * @param args Optional arguments like showPrivileges, showBuiltinRoles
     * @returns Role document or null if not found
     */
    getRole(rolename: string, args?: object): object | null

    /**
     * Get information about all roles in the database.
     * @param args Optional filter and options
     * @returns Array of role documents
     */
    getRoles(args?: object): object[]

    /**
     * Set the default write concern for operations on this database.
     * @param wc Write concern object with w, j, wtimeout fields
     */
    setWriteConcern(wc: object): void

    /**
     * Get the effective write concern for this database.
     * @returns Write concern object or undefined if not set
     */
    getWriteConcern(): object | undefined

    /**
     * Unset (remove) the write concern for this database.
     */
    unsetWriteConcern(): void

    /**
     * Get the current log verbosity levels for all components.
     * @returns Object with verbosity levels by component
     */
    getLogComponents(): object

    /**
     * Set the log verbosity level for a specific component.
     * @param logLevel Verbosity level (-1 to 5, where 0 is default)
     * @param component Component name (e.g., "query", "replication") or omit for global
     * @returns Command result object
     */
    setLogLevel(logLevel: number, component?: string): object

    /**
     * Open a change stream to watch for changes across all collections in the database.
     * @param pipeline Optional aggregation pipeline to filter change events
     * @param options Options like fullDocument, resumeAfter, startAtOperationTime
     * @returns Change stream cursor
     */
    watch(pipeline?: object[], options?: object): DBCommandCursor

    /**
     * Get the implicit session associated with this database connection.
     * @returns The driver session object
     */
    getSession(): DriverSession

    /**
     * Create an encrypted collection using Queryable Encryption (QE).
     * @param name Name of the collection to create
     * @param opts Options including encryptedFields specification
     * @returns Object with collection creation details
     */
    createEncryptedCollection(name: string, opts: object): object

    /**
     * Drop an encrypted collection and its associated state collections.
     * @param name Name of the encrypted collection to drop
     * @returns Command result object
     */
    dropEncryptedCollection(name: string): object

    /**
     * Check for metadata inconsistencies in the database (sharding metadata, indexes, etc.).
     * @param options Options to control the check scope
     * @returns Cursor over inconsistency results
     */
    checkMetadataConsistency(options?: object): DBCommandCursor

    /**
     * Get the shard ID of the primary shard for this database (sharded clusters only).
     * @returns Shard ID string
     */
    getDatabasePrimaryShardId(): string

    /**
     * Get detailed server build information (alias for serverBuildInfo).
     * @returns Object with version, gitVersion, modules, allocator, etc.
     */
    getServerBuildInfo(): object
}
