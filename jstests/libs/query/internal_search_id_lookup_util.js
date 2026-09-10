// $_internalSearchIdLookup is internal-only: rejected in user requests and accepted only from an
// internal client, which must specify an explicit (empty) writeConcern on commands that accept
// one.
export function createInternalClient(hostOrUri) {
    const conn = new Mongo(hostOrUri);
    assert.commandWorked(
        conn.getDB("admin").runCommand({
            hello: 1,
            internalClient: {minWireVersion: NumberInt(0), maxWireVersion: NumberInt(7)},
        }),
    );
    return conn;
}

// Opens an internal-client connection (see createInternalClient) and returns the handle to
// 'dbName' on it, the usual entrypoint for the internalDB the run* helpers below expect.
export function createInternalDB(hostOrUri, dbName) {
    return createInternalClient(hostOrUri).getDB(dbName);
}

// Every $_internalSearchIdLookup command needs readConcern/writeConcern explicitly set for the
// internal-client connection, on top of whatever caller-specific fields (cursor, querySettings,
// shardVersion, ...) the pipeline under test needs.
export function runIdLookupAggregate(internalDB, collectionName, pipeline, extra = {}) {
    return assert.commandWorked(
        internalDB.runCommand({
            aggregate: collectionName,
            pipeline: pipeline,
            cursor: {},
            readConcern: {},
            writeConcern: {},
            ...extra,
        }),
    );
}

// Fans a real document out into one document per entry of 'mockIds', as if $search had handed
// $_internalSearchIdLookup 'mockIds' as its own index results. 'undefined' produces a document
// with no _id field at all. $_internalSearchIdLookup always looks up within the aggregate
// command's own namespace, so this seeds off one real document ($limit: 1) rather than $documents.
export function mockMongotPipeline(mockIds, idLookupSpec = {}) {
    return [
        {$limit: 1},
        {$addFields: {__mongot: mockIds.map((_id) => (_id === undefined ? {} : {_id}))}},
        {$unwind: "$__mongot"},
        {$replaceWith: "$__mongot"},
        {$_internalSearchIdLookup: idLookupSpec},
    ];
}

// Runs $_internalSearchIdLookup against 'collectionName' as if $search had handed it 'mockIds' as
// its own index results. Defaults 'cursor' to {batchSize: 0} so callers needing the raw response
// (e.g. to read cursor.id for a getMore) get an empty firstBatch; DBCommandCursor.toArray()/
// .itcount() still drain the rest via getMore regardless of the initial batchSize. Any further
// options are forwarded onto the aggregate command itself (e.g. shardVersion for direct-to-shard
// runs).
export function runAggWithMockMongot(
    internalDB,
    collectionName,
    mockIds,
    {idLookupSpec = {}, queryKnobs, cursor = {batchSize: 0}, ...extra} = {},
) {
    return runIdLookupAggregate(
        internalDB,
        collectionName,
        mockMongotPipeline(mockIds, idLookupSpec),
        {
            cursor,
            ...(queryKnobs ? {querySettings: {queryKnobs}} : {}),
            ...extra,
        },
    );
}

export function runAggWithMockMongotResults(internalDB, collectionName, mockIds, opts) {
    const res = runAggWithMockMongot(internalDB, collectionName, mockIds, opts);
    return new DBCommandCursor(internalDB, res).toArray();
}

// Reads the search idLookup deltas out of a serverStatus metrics diff, keyed by engine, treating
// any missing leaf as 0. The "aggregation" cell is the classic local-read executor (also used as
// the fallback when SBE declines an _id shape it cannot encode).
export function readIdLookupDelta(delta) {
    const idl = (delta.search && delta.search.idLookup) || {};
    const leaf = (engine, field) => (idl[engine] && idl[engine][field]) || 0;
    const result = {};
    for (const engine of ["sbe", "aggregation"]) {
        result[engine] = {
            found: leaf(engine, "found"),
            notFound: leaf(engine, "notFound"),
            notHandled: leaf(engine, "notHandled"),
        };
    }
    return result;
}
