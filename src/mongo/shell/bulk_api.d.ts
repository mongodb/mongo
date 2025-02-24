// type declarations for bulk_api.js

/**
 * Shell representation of WriteConcern
 */
declare class WriteConcern {

    /**
     * @param  w write waits until replicated to number of servers (including primary), or mode (string)
     * @param  wtimeout how long to wait for "w" replication
     * @param  j write waits for journal
     */
    constructor({ w: wValue, wtimeout : wTimeout, j : jValue}:
        {
            wValue: number | string
            wTimeout: number
            jValue: boolean
        });
    constructor(w: number | string, wtimeout: number, j: boolean);

    toJSON(): object
    tojson(indent, nolint): string
    toString(): string
    shellPrint(): string
}

/**
 * Wraps the result for write commands and presents a convenient api for accessing
 * single results & errors (returns the last one if there are multiple).
 * singleBatchType is passed in on bulk operations consisting of a single batch and
 * are used to filter the WriteResult to only include relevant result fields.
 */
declare class WriteResult  {
    
    constructor(bulkResult, singleBatchType, writeConcern)
    
    readonly ok: any;
    readonly nInserted: number;
    readonly nUpserted: number;
    readonly nMatched: number;
    readonly nModified: number;
    readonly nRemoved: number;

    getUpsertedId();
    getRawResponse();
    getWriteError();
    hasWriteError();
    getWriteConcernError();
    hasWriteConcernError();

    tojson(indent, nolint): string
    toString(): string
    shellPrint(): string
};


/**
 * Wraps the result for the commands
 */
declare class BulkWriteResult {

    constructor(bulkResult, singleBatchType, writeConcern)

    readonly ok: any;
    readonly nInserted: number;
    readonly nUpserted: number;
    readonly nMatched: number;
    readonly nModified: number;
    readonly nRemoved: number;

    getUpsertedId();
    getRawResponse();
    getWriteError();
    hasWriteError();
    getWriteConcernError();
    hasWriteConcernError();

    tojson(indent, nolint): string
    toString(): string
    shellPrint(): string
}

/**
 * Represents a bulk write error, identical to a BulkWriteResult but thrown
 */
declare class BulkWriteError {

    constructor(bulkResult, singleBatchType, writeConcern, message)

    name: string
    message: string
    stack: string

    toString(): string
    toResult(): BulkWriteResult
}

/**
 * Wraps a command error
 */
declare class WriteCommandError {

    constructor(commandError)

    name: string
    message: string
    stack: string

    readonly code
    readonly errmsg

    tojson(indent, nolint): string
    toString(): string
    shellPrint(): string
}

/**
 * Wraps an error for a single write
 */
declare class WriteError {
    constructor(err)

    name: string
    message: string
    stack: string
    
    readonly code
    readonly index
    readonly errmsg

    getOperation()

    tojson(indent, nolint): string
    toString(): string
    shellPrint(): string
}
