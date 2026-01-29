// type declarations for collection.js

/**
 * MongoDB collection object for performing operations on a collection.
 * Accessed via db.collectionName or db.getCollection("collectionName").
 */
declare class DBCollection {
    /**
     * Create a collection object.
     * @param mongo Mongo connection
     * @param db Database object
     * @param shortName Collection name without database prefix
     * @param fullName Full namespace (database.collection)
     */
    constructor(mongo: Mongo, db: DB, shortName: string, fullName: string);

    /**
     * Get the collection name.
     * @returns Collection name
     */
    getName(): string;

    /**
     * Get the full namespace (database.collection).
     * @returns Full namespace string
     */
    getFullName(): string;

    /**
     * Get the Mongo connection object.
     * @returns Mongo connection
     */
    getMongo(): Mongo;

    /**
     * Get the database object.
     * @returns Database object
     */
    getDB(): DB;

    /**
     * Find documents in the collection.
     * @param filter Query filter (default: {})
     * @param projection Fields to include/exclude
     * @param limit Maximum number of documents to return
     * @param skip Number of documents to skip
     * @param batchSize Batch size for cursor
     * @param options Query options
     * @returns Query cursor
     */
    find(filter?: object, projection?: object, limit?: number, skip?: number, batchSize?: number, options?: number): DBQuery;

    /**
     * Find a single document.
     * @param filter Query filter
     * @param projection Fields to include/exclude
     * @param options Query options
     * @param readConcern Read concern level
     * @param collation Collation specification
     * @param rawData Whether to return raw BSON data
     * @returns The matching document or null
     */
    findOne(filter?: object, projection?: object, options?: object, readConcern?: object, collation?: object, rawData?: boolean): any;

    /**
     * Insert a document or documents.
     * @param obj Document or array of documents to insert
     * @param options Insert options (ordered, writeConcern, etc.)
     * @returns WriteResult or BulkWriteResult
     */
    insert(obj: object | object[], options?: object): WriteResult | BulkWriteResult;

    /**
     * Insert a single document.
     * @param doc Document to insert
     * @param options Insert options
     * @returns Insert result
     */
    insertOne(doc: object, options?: object): object;

    /**
     * Insert multiple documents.
     * @param docs Array of documents to insert
     * @param options Insert options
     * @returns Insert result
     */
    insertMany(docs: object[], options?: object): object;

    /**
     * Update documents.
     * @param query Query filter
     * @param updateSpec Update operations or replacement document
     * @param upsert If true, insert if no match found
     * @param multi If true, update all matching documents
     * @returns WriteResult
     */
    update(query: object, updateSpec: object, upsert?: boolean, multi?: boolean): WriteResult;

    /**
     * Update a single document.
     * @param filter Query filter
     * @param update Update operations or replacement document
     * @param options Update options
     * @returns Update result
     */
    updateOne(filter: object, update: object, options?: object): object;

    /**
     * Update multiple documents.
     * @param filter Query filter
     * @param update Update operations
     * @param options Update options
     * @returns Update result
     */
    updateMany(filter: object, update: object, options?: object): object;

    /**
     * Replace a single document.
     * @param filter Query filter
     * @param replacement Replacement document
     * @param options Replace options
     * @returns Replace result
     */
    replaceOne(filter: object, replacement: object, options?: object): object;

    /**
     * Remove documents.
     * @param query Query filter
     * @param justOne If true, remove only the first matching document
     * @returns WriteResult
     */
    remove(query: object, justOne?: boolean): WriteResult;

    /**
     * Delete a single document.
     * @param filter Query filter
     * @param options Delete options
     * @returns Delete result
     */
    deleteOne(filter: object, options?: object): object;

    /**
     * Delete multiple documents.
     * @param filter Query filter
     * @param options Delete options
     * @returns Delete result
     */
    deleteMany(filter: object, options?: object): object;

    /**
     * Save a document (insert if _id doesn't exist, update if it does).
     * @param obj Document to save
     * @param opts Save options
     * @returns WriteResult
     */
    save(obj: object, opts?: object): WriteResult;

    /**
     * Find and atomically modify a document.
     * @param args Arguments object with query, update, sort, remove, new, fields, upsert, etc.
     * @returns The modified document or null
     */
    findAndModify(args: object): any;

    /**
     * Find one document and delete it.
     * @param filter Query filter
     * @param options Options including sort, projection, collation
     * @returns The deleted document or null
     */
    findOneAndDelete(filter: object, options?: object): any;

    /**
     * Find one document and replace it.
     * @param filter Query filter
     * @param replacement Replacement document
     * @param options Options including sort, projection, upsert, returnDocument, collation
     * @returns The document (before or after replacement depending on returnDocument)
     */
    findOneAndReplace(filter: object, replacement: object, options?: object): any;

    /**
     * Find one document and update it.
     * @param filter Query filter
     * @param update Update operations
     * @param options Options including sort, projection, upsert, returnDocument, collation
     * @returns The document (before or after update depending on returnDocument)
     */
    findOneAndUpdate(filter: object, update: object, options?: object): any;

    /**
     * Count documents matching a query.
     * @param query Query filter (default: {})
     * @param options Count options
     * @returns Count of matching documents
     */
    count(query?: object, options?: object): number;

    /**
     * Count documents matching a filter.
     * @param filter Query filter
     * @param options Count options
     * @returns Count result object
     */
    countDocuments(filter?: object, options?: object): number;

    /**
     * Get estimated document count using collection metadata.
     * @param options Options
     * @returns Estimated count
     */
    estimatedDocumentCount(options?: object): number;

    /**
     * Get distinct values for a field.
     * @param key Field name
     * @param query Query filter
     * @param options Options
     * @returns Array of distinct values
     */
    distinct(key: string, query?: object, options?: object): any[];

    /**
     * Run an aggregation pipeline.
     * @param pipeline Aggregation pipeline stages
     * @param options Aggregation options
     * @returns Command cursor with results
     */
    aggregate(pipeline: object[], options?: object): DBCommandCursor;

    /**
     * Create an index on the collection.
     * @param keys Index specification
     * @param options Index options (name, unique, sparse, etc.)
     * @param commitQuorum Commit quorum for index build
     * @param cmdArgs Additional command arguments
     * @returns Command result
     */
    createIndex(keys: object, options?: object, commitQuorum?: any, cmdArgs?: object): object;

    /**
     * Create multiple indexes.
     * @param keys Array of index specifications
     * @param options Index options
     * @param commitQuorum Commit quorum
     * @param cmdArgs Additional command arguments
     * @returns Command result
     */
    createIndexes(keys: object[], options?: object, commitQuorum?: any, cmdArgs?: object): object;

    /**
     * Get all indexes for the collection.
     * @param params Filter parameters
     * @returns Array of index specifications
     */
    getIndexes(params?: object): object[];

    /**
     * Get index specifications as key patterns.
     * @param options Options
     * @returns Array of index key patterns
     */
    getIndexKeys(options?: object): object[];

    /**
     * Get an index by its key pattern.
     * @param keyPattern Index key pattern
     * @param opts Options
     * @returns Index specification or null
     */
    getIndexByKey(keyPattern: object, opts?: object): object | null;

    /**
     * Get an index by its name.
     * @param indexName Index name
     * @param opts Options
     * @returns Index specification or null
     */
    getIndexByName(indexName: string, opts?: object): object | null;

    /**
     * Drop an index.
     * @param index Index name or key pattern
     * @returns Command result
     */
    dropIndex(index: string | object): object;

    /**
     * Drop multiple indexes.
     * @param indexNames Index names or array of names
     * @param cmdArgs Additional command arguments
     * @returns Command result
     */
    dropIndexes(indexNames?: string | string[], cmdArgs?: object): object;

    /**
     * Rebuild all indexes.
     * @returns Command result
     */
    reIndex(): object;

    /**
     * Drop the collection.
     * @param options Drop options
     * @returns Command result
     */
    drop(options?: object): object;

    /**
     * Rename the collection.
     * @param newName New collection name
     * @param dropTarget Whether to drop target if it exists
     * @returns Command result
     */
    renameCollection(newName: string, dropTarget?: boolean): object;

    /**
     * Validate the collection.
     * @param options Validation options (full, background, etc.)
     * @returns Validation result
     */
    validate(options?: object): object;

    /**
     * Get collection statistics.
     * @param scale Scale factor for sizes
     * @returns Statistics object
     */
    stats(scale?: number): object;

    /**
     * Get storage statistics.
     * @param scale Scale factor
     * @returns Storage statistics
     */
    storageSize(scale?: number): number;

    /**
     * Get total index size.
     * @param scale Scale factor
     * @returns Total index size
     */
    totalIndexSize(scale?: number): number;

    /**
     * Get total collection size.
     * @param scale Scale factor
     * @returns Total size
     */
    totalSize(scale?: number): number;

    /**
     * Initialize an ordered bulk operation.
     * @returns Bulk operation builder
     */
    initializeOrderedBulkOp(): object;

    /**
     * Initialize an unordered bulk operation.
     * @returns Bulk operation builder
     */
    initializeUnorderedBulkOp(): object;

    /**
     * Perform a bulk write operation.
     * @param requests Array of write operations
     * @param options Bulk write options
     * @returns Bulk write result
     */
    bulkWrite(requests: object[], options?: object): object;

    /**
     * Watch for changes on the collection.
     * @param pipeline Aggregation pipeline to filter changes
     * @param options Change stream options
     * @returns Change stream cursor
     */
    watch(pipeline?: object[], options?: object): DBCommandCursor;

    /**
     * Get an explainable version of the collection.
     * @param verbosity Explain verbosity level
     * @returns Explainable collection wrapper
     */
    explain(verbosity?: string): Explainable;

    /**
     * Print help for collection methods.
     */
    help(): void;
}
