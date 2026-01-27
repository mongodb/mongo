// type declarations for explainable.js

/**
 * Explainable wrapper for a collection that returns query plans instead of results.
 * Use db.collection.explain(verbosity) to create an explainable collection.
 */
declare class Explainable {
    /**
     * Create an explainable collection wrapper.
     * @param collection The collection to wrap
     * @param verbosity Explain verbosity ("queryPlanner", "executionStats", "allPlansExecution", or boolean)
     */
    constructor(collection: DBCollection, verbosity?: string | boolean);

    /**
     * Get the wrapped collection.
     * @returns The underlying collection
     */
    getCollection(): DBCollection;

    /**
     * Get the explain verbosity level.
     * @returns Verbosity string
     */
    getVerbosity(): string;

    /**
     * Set the explain verbosity level.
     * @param verbosity New verbosity level
     * @returns This explainable for chaining
     */
    setVerbosity(verbosity: string | boolean): Explainable;

    /**
     * Explain a find query.
     * @param filter Query filter
     * @param projection Fields to include/exclude
     * @param limit Maximum number of documents
     * @param skip Number of documents to skip
     * @param batchSize Batch size
     * @param options Query options
     * @returns Explain output
     */
    find(filter?: object, projection?: object, limit?: number, skip?: number, batchSize?: number, options?: number): object;

    /**
     * Explain a findOne query.
     * @param filter Query filter
     * @param projection Fields to include/exclude
     * @param options Query options
     * @param readConcern Read concern
     * @param collation Collation
     * @param rawData Whether to use raw data
     * @returns Explain output
     */
    findOne(filter?: object, projection?: object, options?: object, readConcern?: object, collation?: object, rawData?: boolean): object;

    /**
     * Explain a count query.
     * @param query Query filter
     * @param options Count options
     * @returns Explain output
     */
    count(query?: object, options?: object): object;

    /**
     * Explain a distinct query.
     * @param key Field name
     * @param query Query filter
     * @param options Options
     * @returns Explain output
     */
    distinct(key: string, query?: object, options?: object): object;

    /**
     * Explain an aggregation pipeline.
     * @param pipeline Aggregation pipeline stages
     * @param options Aggregation options
     * @returns Explain output
     */
    aggregate(pipeline: object[], options?: object): object;

    /**
     * Explain an update operation.
     * @param query Query filter
     * @param updateSpec Update operations
     * @param upsert Whether to upsert
     * @param multi Whether to update multiple documents
     * @returns Explain output
     */
    update(query: object, updateSpec: object, upsert?: boolean, multi?: boolean): object;

    /**
     * Explain a remove operation.
     * @param query Query filter
     * @param justOne Whether to remove just one document
     * @returns Explain output
     */
    remove(query: object, justOne?: boolean): object;

    /**
     * Explain a findAndModify operation.
     * @param args Arguments object
     * @returns Explain output
     */
    findAndModify(args: object): object;

    /**
     * Explain a findOneAndDelete operation.
     * @param filter Query filter
     * @param options Options
     * @returns Explain output
     */
    findOneAndDelete(filter: object, options?: object): object;

    /**
     * Explain a findOneAndReplace operation.
     * @param filter Query filter
     * @param replacement Replacement document
     * @param options Options
     * @returns Explain output
     */
    findOneAndReplace(filter: object, replacement: object, options?: object): object;

    /**
     * Explain a findOneAndUpdate operation.
     * @param filter Query filter
     * @param update Update operations
     * @param options Options
     * @returns Explain output
     */
    findOneAndUpdate(filter: object, update: object, options?: object): object;

    /**
     * Print help for explainable methods.
     */
    help(): void;
}
