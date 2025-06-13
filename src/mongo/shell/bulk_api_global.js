// Populate global variables from modules for backwards compatibility

import {
    BulkWriteError,
    BulkWriteResult,
    initializeOrderedBulkOp,
    initializeUnorderedBulkOp,
    WriteCommandError,
    WriteConcern,
    WriteError,
    WriteResult
} from "src/mongo/shell/bulk_api.js";

globalThis.WriteConcern = WriteConcern;
globalThis.WriteResult = WriteResult;
globalThis.BulkWriteResult = BulkWriteResult;
globalThis.BulkWriteError = BulkWriteError;
globalThis.WriteCommandError = WriteCommandError;
globalThis.WriteError = WriteError;

/***********************************************************
 * Adds the initializers of bulk operations to the db collection
 ***********************************************************/
globalThis.DBCollection.prototype.initializeUnorderedBulkOp = initializeUnorderedBulkOp;
globalThis.DBCollection.prototype.initializeOrderedBulkOp = initializeOrderedBulkOp;
