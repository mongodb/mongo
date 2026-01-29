// type declarations for query.js

declare class DBQuery {

    constructor(mongo: Mongo, db: DB, collection: DBCollection, ns: string, filter: object, projection?: object, limit?: number, skip?: number, batchSize?: number, options?: number)

    /**
     * Add a query option flag to the cursor.
     * @param option Option flag to add (e.g., DBQuery.Option.tailable)
     * @returns This cursor for chaining
     */
    addOption(option: number): DBQuery

    /**
     * Allow the query to use temporary disk storage for large sorts.
     * @param value Whether to allow disk use (default: true)
     * @returns This cursor for chaining
     */
    allowDiskUse(value?: boolean): DBQuery

    /**
     * Allow partial results from a sharded cluster if some shards are unavailable.
     * @returns This cursor for chaining
     */
    allowPartialResults(): DBQuery

    /**
     * Access a result document by numeric index.
     * @param index Zero-based index of the document to retrieve
     * @returns The document at the specified index
     */
    arrayAccess(index: number): any

    /**
     * Set the number of documents to return per batch from the server.
     * @param n Batch size (0 = server default)
     * @returns This cursor for chaining
     */
    batchSize(n: number): DBQuery

    /**
     * Create an independent copy of this query cursor.
     * @returns A new DBQuery with the same settings
     */
    clone(): DBQuery

    /**
     * Close the cursor and free server resources.
     */
    close(): void

    /**
     * Specify collation rules for string comparison.
     * @param collationSpec Collation specification object
     * @returns This cursor for chaining
     */
    collation(collationSpec: object): DBQuery

    /**
     * Attach a comment to the query for identification in profiler and logs.
     * @param comment Comment string to attach
     * @returns This cursor for chaining
     */
    comment(comment: string): DBQuery

    /**
     * Count the number of documents matching the query.
     * @param applySkipLimit If true, applies skip and limit modifiers
     * @returns The count of matching documents
     */
    count(applySkipLimit?: boolean): number

    /**
     * Get the query execution plan and statistics.
     * @param verbose Verbosity level: "queryPlanner", "executionStats", "allPlansExecution", or boolean
     * @returns Explain output object
     */
    explain(verbose?: string | boolean): object

    /**
     * Execute a function for each document in the result set.
     * @param func Callback function to execute for each document
     */
    forEach(func: (doc: any) => void): void

    /**
     * Get the cluster time from the last operation (for snapshot reads).
     * @returns The cluster time timestamp, or undefined if not available
     */
    getClusterTime(): Timestamp | undefined

    /**
     * Get the server-side cursor ID.
     * @returns The cursor ID as a NumberLong
     */
    getId(): NumberLong

    /**
     * Check if there are more documents available in the cursor.
     * @returns True if more documents are available
     */
    hasNext(): boolean

    /**
     * Print help documentation for cursor methods.
     */
    help(): void

    /**
     * Suggest a specific index for the query optimizer to use.
     * @param index Index name (string) or index specification (object)
     * @returns This cursor for chaining
     */
    hint(index: string | object): DBQuery

    /**
     * Check if the cursor has been closed.
     * @returns True if the cursor is closed
     */
    isClosed(): boolean

    /**
     * Check if all results have been retrieved from the server.
     * @returns True if no more documents are available
     */
    isExhausted(): boolean

    /**
     * Iterate through all documents and return the count.
     * @returns The number of documents iterated
     */
    itcount(): number

    /**
     * Get the total number of documents (materializes all results into memory).
     * @returns The length of the result array
     */
    length(): number

    /**
     * Limit the maximum number of documents to return.
     * @param n Maximum number of documents (0 = no limit)
     * @returns This cursor for chaining
     */
    limit(n: number): DBQuery

    /**
     * Transform each document using a mapping function.
     * @param func Function to apply to each document
     * @returns Array of transformed results
     */
    map<T>(func: (doc: any) => T): T[]

    /**
     * Set the maximum index key values for a range query.
     * @param indexBounds Maximum bounds object matching the index
     * @returns This cursor for chaining
     */
    max(indexBounds: object): DBQuery

    /**
     * Set the maximum execution time for the query.
     * @param ms Maximum time in milliseconds
     * @returns This cursor for chaining
     */
    maxTimeMS(ms: number): DBQuery

    /**
     * Set the minimum index key values for a range query.
     * @param indexBounds Minimum bounds object matching the index
     * @returns This cursor for chaining
     */
    min(indexBounds: object): DBQuery

    /**
     * Get the next document from the cursor.
     * @returns The next document, or throws if no more documents
     */
    next(): any

    /**
     * Prevent the cursor from timing out after inactivity.
     * @returns This cursor for chaining
     */
    noCursorTimeout(): DBQuery

    /**
     * Get the number of documents remaining in the current batch.
     * @returns Count of documents left in batch
     */
    objsLeftInBatch(): number

    /**
     * Enable pretty-printing for better readability in the shell.
     * @returns This cursor for chaining
     */
    pretty(): DBQuery

    /**
     * Specify which fields to include or exclude in results.
     * @param spec Projection specification object
     * @returns This cursor for chaining
     */
    projection(spec: object): DBQuery

    /**
     * Return results in raw BSON binary format.
     * @returns This cursor for chaining
     */
    rawData(): DBQuery

    /**
     * Set the read concern level for the query.
     * @param level Read concern level ("local", "majority", "linearizable", "snapshot", or "available")
     * @param atClusterTime Optional cluster time for snapshot reads
     * @returns This cursor for chaining
     */
    readConcern(level: string, atClusterTime?: Timestamp): DBQuery

    /**
     * Mark the cursor as read-only for optimization.
     * @returns This cursor for chaining
     */
    readOnly(): DBQuery

    /**
     * Set the read preference for replica set queries.
     * @param mode Read preference mode ("primary", "primaryPreferred", "secondary", "secondaryPreferred", "nearest")
     * @param tagSet Optional array of tag sets for server selection
     * @returns This cursor for chaining
     */
    readPref(mode: string, tagSet?: object[]): DBQuery

    /**
     * Return only the index keys instead of full documents.
     * @returns This cursor for chaining
     */
    returnKey(): DBQuery

    /**
     * Get a string representation suitable for shell output.
     * @returns String representation of cursor results
     */
    shellPrint(): string

    /**
     * @deprecated Use showRecordId() instead
     * Include disk location information in results.
     * @returns This cursor for chaining
     */
    showDiskLoc(): DBQuery

    /**
     * Include the internal $recordId field in result documents.
     * @returns This cursor for chaining
     */
    showRecordId(): DBQuery

    /**
     * Count documents with skip and limit applied.
     * @returns The count after applying skip/limit
     */
    size(): number

    /**
     * Skip a specified number of documents.
     * @param n Number of documents to skip
     * @returns This cursor for chaining
     */
    skip(n: number): DBQuery

    /**
     * Sort results by the specified fields.
     * @param sortBy Sort specification object (e.g., {field: 1} for ascending)
     * @returns This cursor for chaining
     */
    sort(sortBy: object): DBQuery

    /**
     * Create a tailable cursor that remains open after reaching the end (for capped collections).
     * @param awaitData If true, block briefly waiting for new data before returning
     * @returns This cursor for chaining
     */
    tailable(awaitData?: boolean): DBQuery

    /**
     * Materialize all cursor results into an array.
     * @returns Array containing all result documents
     */
    toArray(): any[]

    /**
     * Get a string representation of the cursor.
     * @returns String representation
     */
    toString(): string
}
/**
 * Cursor returned by database commands like aggregate() and watch().
 * Unlike DBQuery, this cursor type is used for commands that return a cursor.
 */
declare class DBCommandCursor {
    /**
     * Close the cursor and release server resources.
     */
    close(): void

    /**
     * Execute a function for each document in the cursor.
     * @param func Callback function to execute for each document
     */
    forEach(func: (doc: any) => void): void

    /**
     * Get the change stream protocol version (for change stream cursors).
     * @returns The version string ("v1" or "v2"), or undefined if not a change stream
     */
    getChangeStreamVersion(): string | undefined

    /**
     * Get the cluster time from the most recent operation.
     * @returns The cluster time timestamp, or undefined if not available
     */
    getClusterTime(): Timestamp | undefined

    /**
     * Get the server-side cursor ID.
     * @returns The cursor ID as a NumberLong
     */
    getId(): NumberLong

    /**
     * Get the resume token for change streams (used to resume after disconnect).
     * @returns The resume token object, or undefined if not a change stream
     */
    getResumeToken(): object | undefined

    /**
     * Check if there are more documents available in the cursor.
     * @returns True if more documents are available
     */
    hasNext(): boolean

    /**
     * Print help documentation for cursor methods.
     */
    help(): void

    /**
     * Check if the cursor has been closed.
     * @returns True if the cursor is closed
     */
    isClosed(): boolean

    /**
     * Check if all results have been retrieved from the server.
     * @returns True if no more documents are available
     */
    isExhausted(): boolean

    /**
     * Iterate through all documents and return the count.
     * @returns The number of documents iterated
     */
    itcount(): number

    /**
     * Transform each document using a mapping function.
     * @param func Function to apply to each document
     * @returns Array of transformed results
     */
    map<T>(func: (doc: any) => T): T[]

    /**
     * Get the next document from the cursor.
     * @returns The next document, or throws if no more documents
     */
    next(): any

    /**
     * Get the number of documents remaining in the current batch.
     * @returns Count of documents left in batch
     */
    objsLeftInBatch(): number

    /**
     * Enable pretty-printing for better readability in the shell.
     * @returns This cursor for chaining
     */
    pretty(): DBCommandCursor

    /**
     * Get a string representation suitable for shell output.
     * @returns String representation of cursor results
     */
    shellPrint(): string

    /**
     * Materialize all cursor results into an array.
     * @returns Array containing all result documents
     */
    toArray(): any[]
}

/**
 * Helper functions for working with queries and cursors.
 */
declare module QueryHelpers {
    /**
     * Get the resume token from the most recent query operation.
     * @returns The resume token object, or undefined if not available
     */
    function getResumeToken(): object | undefined
}

/**
 * Shell variable holding the last query result for iteration.
 * Used by the shell to store results when typing `it` to iterate.
 */
declare var ___it___: any
