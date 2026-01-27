// type declarations for bulk_api.js

/**
 * Write concern specification for write operations.
 */
declare class WriteConcern {
    /**
     * Create a write concern.
     * @param wValue Write concern w value (number, "majority", or tag set)
     * @param wTimeout Timeout in milliseconds
     * @param jValue Whether to wait for journal commit
     */
    constructor(wValue: any, wTimeout?: number, jValue?: boolean);
}

/**
 * Result of a write operation (insert, update, remove).
 */
declare class WriteResult {
    /** Whether the operation succeeded */
    readonly ok: number;
    /** Number of documents inserted */
    readonly nInserted: number;
    /** Number of documents upserted */
    readonly nUpserted: number;
    /** Number of documents matched */
    readonly nMatched: number;
    /** Number of documents modified */
    readonly nModified: number;
    /** Number of documents removed */
    readonly nRemoved: number;
    /** ID of upserted document (if applicable) */
    readonly _id?: any;

    /**
     * Get the upserted document ID.
     * @returns Upserted ID object or null
     */
    getUpsertedId(): object | null;

    /**
     * Get the raw response from the server.
     * @returns Raw bulk result object
     */
    getRawResponse(): object;

    /**
     * Get the write error if one occurred.
     * @returns Write error object or null
     */
    getWriteError(): object | null;

    /**
     * Check if a write error occurred.
     * @returns True if write error exists
     */
    hasWriteError(): boolean;

    /**
     * Get the write concern error if one occurred.
     * @returns Write concern error object or null
     */
    getWriteConcernError(): object | null;

    /**
     * Check if a write concern error occurred.
     * @returns True if write concern error exists
     */
    hasWriteConcernError(): boolean;

    /**
     * Convert to JSON string.
     * @param indent Indentation string
     * @param nolint Whether to output single line
     * @returns JSON string
     */
    tojson(indent?: string, nolint?: boolean): string;

    /**
     * Convert to string.
     * @returns String representation
     */
    toString(): string;

    /**
     * Shell print representation.
     * @returns String for shell printing
     */
    shellPrint(): string;
}

/**
 * Result of a bulk write operation.
 */
declare class BulkWriteResult {
    /** Whether the operation succeeded */
    readonly ok: number;
    /** Number of documents inserted */
    readonly nInserted: number;
    /** Number of documents upserted */
    readonly nUpserted: number;
    /** Number of documents matched */
    readonly nMatched: number;
    /** Number of documents modified */
    readonly nModified: number;
    /** Number of documents removed */
    readonly nRemoved: number;

    /**
     * Get array of upserted documents.
     * @returns Array of upserted IDs
     */
    getUpsertedIds(): object[];

    /**
     * Get upserted document ID at index.
     * @param index Index of upserted document
     * @returns Upserted ID object or undefined
     */
    getUpsertedIdAt(index: number): object | undefined;

    /**
     * Get array of write errors.
     * @returns Array of write errors
     */
    getWriteErrors(): object[];

    /**
     * Get write error at index.
     * @param index Index of write error
     * @returns Write error object or undefined
     */
    getWriteErrorAt(index: number): object | undefined;

    /**
     * Get count of write errors.
     * @returns Number of write errors
     */
    getWriteErrorCount(): number;

    /**
     * Check if write errors occurred.
     * @returns True if write errors exist
     */
    hasWriteErrors(): boolean;

    /**
     * Get write concern error if one occurred.
     * @returns Write concern error object or null
     */
    getWriteConcernError(): object | null;

    /**
     * Check if write concern error occurred.
     * @returns True if write concern error exists
     */
    hasWriteConcernError(): boolean;

    /**
     * Convert to JSON string.
     * @param indent Indentation string
     * @param nolint Whether to output single line
     * @returns JSON string
     */
    tojson(indent?: string, nolint?: boolean): string;

    /**
     * Convert to string.
     * @returns String representation
     */
    toString(): string;

    /**
     * Shell print representation.
     * @returns String for shell printing
     */
    shellPrint(): string;
}

/**
 * Error result from a bulk write operation.
 * Extends BulkWriteResult with error information.
 */
declare class BulkWriteError extends BulkWriteResult {
    /** Error name */
    name: string;
    /** Error message */
    message: string;
    /** Stack trace */
    stack: string;

    /**
     * Convert to string.
     * @returns Error string
     */
    toString(): string;
}

/**
 * Error from a write command.
 */
declare class WriteCommandError {
    /** Error code */
    readonly code: number;
    /** Error message */
    readonly errmsg: string;
    /** Additional error info */
    readonly errInfo?: object;

    /**
     * Get error code.
     * @returns Error code
     */
    getCode(): number;

    /**
     * Convert to JSON string.
     * @param indent Indentation string
     * @param nolint Whether to output single line
     * @returns JSON string
     */
    tojson(indent?: string, nolint?: boolean): string;

    /**
     * Convert to string.
     * @returns String representation
     */
    toString(): string;

    /**
     * Shell print representation.
     * @returns String for shell printing
     */
    shellPrint(): string;
}

/**
 * Write error from an individual write operation.
 */
declare class WriteError {
    /** Error name */
    name: string;
    /** Error message */
    message: string;
    /** Error code */
    code: number;
    /** Index of failed operation */
    index: number;
    /** Error message from server */
    errmsg: string;
    /** Operation that failed */
    op: object;

    /**
     * Get error code.
     * @returns Error code
     */
    getCode(): number;

    /**
     * Convert to JSON string.
     * @returns JSON string
     */
    tojson(): string;

    /**
     * Convert to string.
     * @returns String representation
     */
    toString(): string;
}

/**
 * Write concern error.
 */
declare class WriteConcernError {
    /** Error code */
    code: number;
    /** Error code name */
    codeName: string;
    /** Error message */
    errmsg: string;
    /** Additional error info */
    errInfo?: object;

    /**
     * Convert to JSON string.
     * @returns JSON string
     */
    tojson(): string;

    /**
     * Convert to string.
     * @returns String representation
     */
    toString(): string;
}
