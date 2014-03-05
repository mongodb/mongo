//
// Scope for the function
//
var _bulk_api_module = (function() {
  // Batch types
  var NONE = 0;
  var INSERT = 1;
  var UPDATE = 2;
  var REMOVE = 3

  // Error codes
  var UNKNOWN_ERROR = 8;
  var WRITE_CONCERN_FAILED = 64;
  var UNKNOWN_REPL_WRITE_CONCERN = 79;
  var NOT_MASTER = 10107;

  // Constants
  var IndexCollPattern = new RegExp('system\.indexes$');

  /**
   * Helper function to define properties
   */
  var defineReadOnlyProperty = function(self, name, value) {
    Object.defineProperty(self, name, {
        enumerable: true
      , get: function() {
        return value;
      }
    });
  }

  /**
   * getLastErrorMethod that supports all write concerns
   */
  var executeGetLastError = function(db, options) {
    var cmd = { getlasterror : 1 };
    options = options || {};

    // Add write concern options to the command
    if(typeof(options.w) != 'undefined') cmd.w = options.w;
    if(typeof(options.wtimeout) != 'undefined') cmd.wtimeout = options.wtimeout;
    if(options.j) cmd.j = options.j;
    if(options.fsync) cmd.fsync = options.fsync;

    // Execute the getLastErrorCommand
    return db.runCommand( cmd );
  };

  /**
   * Wraps the result for write commands and presents a convenient api for accessing
   * single results & errors (returns the last one if there are multiple).
   * singleBatch is passed in on bulk operations consisting of a single batch and
   * are used to filter the SingleWriteResult to only include relevant result fields.
   */
  var SingleWriteResult = function(bulkResult, singleBatch, writeConcern) {
    // Define properties
    defineReadOnlyProperty(this, "ok", bulkResult.ok);
    defineReadOnlyProperty(this, "nInserted", bulkResult.nInserted);
    defineReadOnlyProperty(this, "nUpserted", bulkResult.nUpserted);
    defineReadOnlyProperty(this, "nMatched", bulkResult.nMatched);
    defineReadOnlyProperty(this, "nModified", bulkResult.nModified);
    defineReadOnlyProperty(this, "nRemoved", bulkResult.nRemoved);

    //
    // Define access methods
    this.getUpsertedId = function() {
      if (bulkResult.upserted.length == 0) {
        return null;
      }

      return bulkResult.upserted[bulkResult.upserted.length - 1];
    };

    this.getRawResponse = function() {
      return bulkResult;
    };

    this.hasWriteErrors = function() {
      return bulkResult.writeErrors.length > 0;
    };

    this.getWriteError = function() {
      return bulkResult.writeErrors[bulkResult.writeErrors.length - 1];
    };

    this.getWriteConcernError = function() {
      if (bulkResult.writeConcernErrors.length == 0) {
        return null;
      } else {
        return bulkResult.writeConcernErrors[0];
      }
    };

    /**
     * @return {string}
     */
    this.tojson = function(indent, nolint) {
      var result = {}

      if(singleBatch && singleBatch.batchType == INSERT) {
        result.nInserted = this.nInserted;
      }

      if(singleBatch && singleBatch.batchType == UPDATE) {
        result.nMatched = this.nMatched;
        result.nUpserted = this.nUpserted;
        
        if(this.nModified != undefined)
            result.nModified = this.nModified;
        
        if(Array.isArray(bulkResult.upserted)
            && bulkResult.upserted.length == 1) {
          result._id = bulkResult.upserted[0]._id;
        }
      }

      if(singleBatch && singleBatch.batchType == REMOVE) {
        result.nRemoved = bulkResult.nRemoved;
      }

      if(this.getWriteError() != null) {
        result.writeError = {};
        result.writeError.code = this.getWriteError().code;
        result.writeError.errmsg = this.getWriteError().errmsg;
      }

      if(this.getWriteConcernError() != null) {
        result.writeConcernError = this.getWriteConcernError();
      }

      return tojson(result, indent, nolint);
    };

    this.toString = function() {
      // Suppress all output for the write concern w:0, since the client doesn't care.
      if(writeConcern && writeConcern.w == 0) {
        return "WriteResult(" + tojson({}) + ")";;
      }
      return "WriteResult(" + this.tojson() + ")";
    };

    this.shellPrint = function() {
      return this.toString();
    };

    this.isOK = function() {
      return bulkResult.ok == 1;
    };
  };

  /**
   * Wraps the result for the commands
   */
  var BulkWriteResult = function(bulkResult, singleBatch, writeConcern) {
    // Define properties
    defineReadOnlyProperty(this, "ok", bulkResult.ok);
    defineReadOnlyProperty(this, "nInserted", bulkResult.nInserted);
    defineReadOnlyProperty(this, "nUpserted", bulkResult.nUpserted);
    defineReadOnlyProperty(this, "nMatched", bulkResult.nMatched);
    defineReadOnlyProperty(this, "nModified", bulkResult.nModified);
    defineReadOnlyProperty(this, "nRemoved", bulkResult.nRemoved);

    //
    // Define access methods
    this.getUpsertedIds = function() {
      return bulkResult.upserted;
    }

    this.getUpsertedIdAt = function(index) {
      return bulkResult.upserted[index];
    }

    this.getRawResponse = function() {
      return bulkResult;
    }

    this.hasWriteErrors = function() {
      return bulkResult.writeErrors.length > 0;
    }

    this.getWriteErrorCount = function() {
      return bulkResult.writeErrors.length;
    }

    this.getWriteErrorAt = function(index) {
      if(index < bulkResult.writeErrors.length) {
        return bulkResult.writeErrors[index];
      }
      return null;
    }

    //
    // Get all errors
    this.getWriteErrors = function() {
      return bulkResult.writeErrors;
    }

    this.getWriteConcernError = function() {
      if(bulkResult.writeConcernErrors.length == 0) {
        return null;
      } else if(bulkResult.writeConcernErrors.length == 1) {
        // Return the error
        return bulkResult.writeConcernErrors[0];
      } else {

        // Combine the errors
        var errmsg = "";
        for(var i = 0; i < bulkResult.writeConcernErrors.length; i++) {
          var err = bulkResult.writeConcernErrors[i];
          errmsg = errmsg + err.errmsg;
          // TODO: Something better
          if (i != bulkResult.writeConcernErrors.length - 1) {
            errmsg = errmsg + " and ";
          }
        }

        return new WriteConcernError({ errmsg : errmsg, code : WRITE_CONCERN_FAILED });
      }
    }

    /**
     * @return {string}
     */
    this.tojson = function(indent, nolint) {
      return tojson(bulkResult, indent, nolint);
    }

    this.toString = function() {
      // Suppress all output for the write concern w:0, since the client doesn't care.
      if(writeConcern && writeConcern.w == 0) {
        return "BulkWriteResult(" + tojson({}) + ")";;
      }
      return "BulkWriteResult(" + this.tojson() + ")";
    }

    this.shellPrint = function() {
      return this.toString();
    }

    this.isOK = function() {
      return bulkResult.ok == 1;
    };

    /**
     * @return {SingleWriteResult} the simplified results condensed into one.
     */
    this.toSingleResult = function() {
      if(singleBatch == null) throw Error(
          "Cannot output SingleWriteResult from multiple batch result");
      return new SingleWriteResult(bulkResult, singleBatch, writeConcern);
    }
  };

  /**
   * Wraps the error
   */
  var WriteError = function(err) {
    if(!(this instanceof WriteError)) return new WriteError(err);

    // Define properties
    defineReadOnlyProperty(this, "code", err.code);
    defineReadOnlyProperty(this, "index", err.index);
    defineReadOnlyProperty(this, "errmsg", err.errmsg);

    //
    // Define access methods
    this.getOperation = function() {
      return err.op;
    }

    /**
     * @return {string}
     */
    this.tojson = function(indent, nolint) {
      return tojson(err, indent, nolint);
    }

    this.toString = function() {
      return "WriteError(" + tojson(err) + ")";
    }

    this.shellPrint = function() {
      return this.toString();
    }
  }

  /**
   * Wraps a write concern error
   */
  var WriteConcernError = function(err) {
    if(!(this instanceof WriteConcernError)) return new WriteConcernError(err);

    // Define properties
    defineReadOnlyProperty(this, "code", err.code);
    defineReadOnlyProperty(this, "errInfo", err.errInfo);
    defineReadOnlyProperty(this, "errmsg", err.errmsg);

    /**
     * @return {string}
     */
    this.tojson = function(indent, nolint) {
      return tojson(err, indent, nolint);
    }

    this.toString = function() {
      return "WriteConcernError(" + tojson(err) + ")";
    }

    this.shellPrint = function() {
      return this.toString();
    }
  }

  /**
   * Keeps the state of an unordered batch so we can rewrite the results
   * correctly after command execution
   */
  var Batch = function(batchType, originalZeroIndex) {
    this.originalZeroIndex = originalZeroIndex;
    this.batchType = batchType;
    this.operations = [];
  }

  /**
   * Wraps a legacy operation so we can correctly rewrite its error
   */
  var LegacyOp = function(batchType, operation, index) {
    this.batchType = batchType;
    this.index = index;
    this.operation = operation;
  }

  /***********************************************************
   * Adds the initializers of bulk operations to the db collection
   ***********************************************************/
  DBCollection.prototype.initializeUnorderedBulkOp = function() {
    return new Bulk(this, false);
  }

  DBCollection.prototype.initializeOrderedBulkOp = function() {
    return new Bulk(this, true);
  }

  /***********************************************************
   * Wraps the operations done for the batch
   ***********************************************************/
  var Bulk = function(collection, ordered) {
    var self = this;
    var coll = collection;
    var executed = false;

    // Set max byte size
    var maxBatchSizeBytes = 1024 * 1024 * 16;
    var maxNumberOfDocsInBatch = 1000;
    var writeConcern = null;
    var currentOp;

    // Final results
    var bulkResult = {
        writeErrors: []
      , writeConcernErrors: []
      , nInserted: 0
      , nUpserted: 0
      , nMatched: 0
      , nModified: 0
      , nRemoved: 0
      , upserted: []
    };

    // Current batch
    var currentBatch = null;
    var currentIndex = 0;
    var currentBatchSize = 0;
    var currentBatchSizeBytes = 0;
    var batches = [];

    var defineBatchTypeCounter = function(self, name, type) {
      Object.defineProperty(self, name, {
          enumerable: true
        , get: function() {
          var counter = 0;

          for(var i = 0; i < batches.length; i++) {
            if(batches[i].batchType == type) {
              counter += batches[i].operations.length;
            }
          }

          if(currentBatch && currentBatch.batchType == type) {
            counter += currentBatch.operations.length;
          }

          return counter;
        }
      });
    }

    defineBatchTypeCounter(this, "nInsertOps", INSERT);
    defineBatchTypeCounter(this, "nUpdateOps", UPDATE);
    defineBatchTypeCounter(this, "nRemoveOps", REMOVE);

    // Convert bulk into string
    this.toString = function() {
      return this.tojson();
    }

    this.tojson = function() {
      return tojson({
          nInsertOps: this.nInsertOps
        , nUpdateOps: this.nUpdateOps
        , nRemoveOps: this.nRemoveOps
        , nBatches: batches.length + (currentBatch == null ? 0 : 1)
      })
    }

    this.getOperations = function() {
      return batches;
    }

    var finalizeBatch = function(newDocType) {
        // Save the batch to the execution stack
        batches.push(currentBatch);

        // Create a new batch
        currentBatch = new Batch(newDocType, currentIndex);

        // Reset the current size trackers
        currentBatchSize = 0;
        currentBatchSizeBytes = 0;
    };

    // Add to internal list of documents
    var addToOperationsList = function(docType, document) {
      // Get the bsonSize
      var bsonSize = Object.bsonsize(document);
      // Create a new batch object if we don't have a current one
      if(currentBatch == null) currentBatch = new Batch(docType, currentIndex);

      // Update current batch size
      currentBatchSize = currentBatchSize + 1;
      currentBatchSizeBytes = currentBatchSizeBytes + bsonSize;

      // Finalize and create a new batch if we have a new operation type
      if (currentBatch.batchType != docType) {
          finalizeBatch(docType);
      }

      // We have an array of documents
      if(Array.isArray(document)) {
        throw Error("operation passed in cannot be an Array");
      } else {
        currentBatch.operations.push(document)
        currentIndex = currentIndex + 1;
      }

      // Check if the batch exceeds one of the size limits
      if((currentBatchSize >= maxNumberOfDocsInBatch)
        || (currentBatchSizeBytes >= maxBatchSizeBytes)) {
          finalizeBatch(docType);
      }

    };

    /**
     * @return {Object} a new document with an _id: ObjectId if _id is not present.
     *     Otherwise, returns the same object passed.
     */
    var addIdIfNeeded = function(obj) {
      if ( typeof( obj._id ) == "undefined" && ! Array.isArray( obj ) ){
        var tmp = obj; // don't want to modify input
        obj = {_id: new ObjectId()};
        for (var key in tmp){
          obj[key] = tmp[key];
        }
      }

      return obj;
    };

    /**
     * Add the insert document.
     *
     * @param document {Object} the document to insert.
     */
    this.insert = function(document) {
      if (!IndexCollPattern.test(coll.getName())) {
        collection._validateForStorage(document);
      }

      return addToOperationsList(INSERT, document);
    };

    //
    // Find based operations
    var findOperations = {
      update: function(updateDocument) {
        collection._validateUpdateDoc(updateDocument);

        // Set the top value for the update 0 = multi true, 1 = multi false
        var upsert = typeof currentOp.upsert == 'boolean' ? currentOp.upsert : false;
        // Establish the update command
        var document = {
            q: currentOp.selector
          , u: updateDocument
          , multi: true
          , upsert: upsert
        }

        // Clear out current Op
        currentOp = null;
        // Add the update document to the list
        return addToOperationsList(UPDATE, document);
      },

      updateOne: function(updateDocument) {
        collection._validateUpdateDoc(updateDocument);

        // Set the top value for the update 0 = multi true, 1 = multi false
        var upsert = typeof currentOp.upsert == 'boolean' ? currentOp.upsert : false;
        // Establish the update command
        var document = {
            q: currentOp.selector
          , u: updateDocument
          , multi: false
          , upsert: upsert
        }

        // Clear out current Op
        currentOp = null;
        // Add the update document to the list
        return addToOperationsList(UPDATE, document);
      },

      replaceOne: function(updateDocument) {
        findOperations.updateOne(updateDocument);
      },

      upsert: function() {
        currentOp.upsert = true;
        // Return the findOperations
        return findOperations;
      },

      removeOne: function() {
        collection._validateRemoveDoc(currentOp.selector);

        // Establish the removeOne command
        var document = {
            q: currentOp.selector
          , limit: 1
        }

        // Clear out current Op
        currentOp = null;
        // Add the remove document to the list
        return addToOperationsList(REMOVE, document);
      },

      remove: function() {
        collection._validateRemoveDoc(currentOp.selector);

        // Establish the remove command
        var document = {
            q: currentOp.selector
          , limit: 0
        }

        // Clear out current Op
        currentOp = null;
        // Add the remove document to the list
        return addToOperationsList(REMOVE, document);
      }
    }

    //
    // Start of update and remove operations
    this.find = function(selector) {
      if (selector == undefined) throw Error("find() requires query criteria");
      // Save a current selector
      currentOp = {
        selector: selector
      }

      // Return the find Operations
      return findOperations;
    }

    //
    // Merge write command result into aggregated results object
    var mergeBatchResults = function(batch, bulkResult, result) {
      //
      // NEEDED to pass tests as some write errors are
      // returned as write concern errors (j write on non journal mongod)
      // also internal error code 75 is still making it out as a write concern error
      //
      if(ordered && result && result.writeConcernError
        && (result.writeConcernError.code == 2 || result.writeConcernError.code == 75)) {
        throw Error(
            "legacy batch failed, cannot aggregate results: "
                + result.writeConcernError.errmsg);
      }

      // If we have an insert Batch type
      if(batch.batchType == INSERT) {
        bulkResult.nInserted = bulkResult.nInserted + result.n;
      }

      // If we have a remove batch type
      if(batch.batchType == REMOVE) {
        bulkResult.nRemoved = bulkResult.nRemoved + result.n;
      }

      var nUpserted = 0;

      // We have an array of upserted values, we need to rewrite the indexes
      if(Array.isArray(result.upserted)) {

        nUpserted = result.upserted.length;

        for(var i = 0; i < result.upserted.length; i++) {
          bulkResult.upserted.push({
              index: result.upserted[i].index + batch.originalZeroIndex
            , _id: result.upserted[i]._id
          });
        }
      } else if(result.upserted) {

        nUpserted = 1;

        bulkResult.upserted.push({
            index: batch.originalZeroIndex
          , _id: result.upserted
        });
      }

      // If we have an update Batch type
      if(batch.batchType == UPDATE) {
        bulkResult.nUpserted = bulkResult.nUpserted + nUpserted;
        bulkResult.nMatched = bulkResult.nMatched + (result.n - nUpserted);
        if(result.nModified == undefined) {
            bulkResult.nModified = undefined;
        } else if(bulkResult.nModified != undefined) {
            bulkResult.nModified = bulkResult.nModified + result.nModified;
        }
      }

      if(Array.isArray(result.writeErrors)) {
        for(var i = 0; i < result.writeErrors.length; i++) {

          var writeError = {
              index: batch.originalZeroIndex + result.writeErrors[i].index
            , code: result.writeErrors[i].code
            , errmsg: result.writeErrors[i].errmsg
            , op: batch.operations[result.writeErrors[i].index]
          };

          bulkResult.writeErrors.push(new WriteError(writeError));
        }
      }

      if(result.writeConcernError) {
        bulkResult.writeConcernErrors.push(new WriteConcernError(result.writeConcernError));
      }
    }

    //
    // Execute the batch
    var executeBatch = function(batch) {
      var cmd = null;
      var result = null;

      // Generate the right update
      if(batch.batchType == UPDATE) {
        cmd = { update: coll.getName(), updates: batch.operations, ordered: ordered }
      } else if(batch.batchType == INSERT) {
        var transformedInserts = [];
        batch.operations.forEach(function(insertDoc) {
          transformedInserts.push(addIdIfNeeded(insertDoc));
        });
        batch.operations = transformedInserts;

        cmd = { insert: coll.getName(), documents: batch.operations, ordered: ordered }
      } else if(batch.batchType == REMOVE) {
        cmd = { delete: coll.getName(), deletes: batch.operations, ordered: ordered }
      }

      // If we have a write concern
      if(writeConcern) {
        cmd.writeConcern = writeConcern;
      }

      // Run the command (may throw)

      // Get command collection
      var cmdColl = collection._db.getCollection('$cmd');
      // Bypass runCommand to ignore slaveOk and read pref settings
      result = new DBQuery(collection.getMongo(), collection._db,
                           cmdColl, cmdColl.getFullName(), cmd,
                           {} /* proj */, -1 /* limit */, 0 /* skip */, 0 /* batchSize */,
                           0 /* flags */).next();

      if(result.ok == 0) {
        throw Error(
            "batch failed, cannot aggregate results: " + result.errmsg);
      }

      // Merge the results
      mergeBatchResults(batch, bulkResult, result);
    }

    // Execute a single legacy op
    var executeLegacyOp = function(_legacyOp) {
      // Handle the different types of operation types
      if(_legacyOp.batchType == INSERT) {
        if (Array.isArray(_legacyOp.operation)) {
          var transformedInserts = [];
          _legacyOp.operation.forEach(function(insertDoc) {
            transformedInserts.push(addIdIfNeeded(insertDoc));
          });
          _legacyOp.operation = transformedInserts;
        }
        else {
          _legacyOp.operation = addIdIfNeeded(_legacyOp.operation);
        }

        collection.getMongo().insert(collection.getFullName(),
                                     _legacyOp.operation,
                                     ordered);
      } else if(_legacyOp.batchType == UPDATE) {
        collection.getMongo().update(collection.getFullName(),
                                     _legacyOp.operation.q,
                                     _legacyOp.operation.u,
                                     _legacyOp.operation.upsert,
                                     _legacyOp.operation.multi);
      } else if(_legacyOp.batchType == REMOVE) {
        var single = Boolean(_legacyOp.operation.limit);

        collection.getMongo().remove(collection.getFullName(),
                                     _legacyOp.operation.q,
                                     single);
      }
    }

    /**
     * Parses the getLastError response and properly sets the write errors and
     * write concern errors.
     * Should kept be up to date with BatchSafeWriter::extractGLEErrors.
     *
     * @return {object} an object with the format:
     *
     * {
     *   writeError: {object|null} raw write error object without the index.
     *   wcError: {object|null} raw write concern error object.
     * }
     */
    var extractGLEErrors = function(gleResponse) {
      var isOK = gleResponse.ok? true : false;
      var err = (gleResponse.err)? gleResponse.err : '';
      var errMsg = (gleResponse.errmsg)? gleResponse.errmsg : '';
      var wNote = (gleResponse.wnote)? gleResponse.wnote : '';
      var jNote = (gleResponse.jnote)? gleResponse.jnote : '';
      var code = gleResponse.code;
      var timeout = gleResponse.wtimeout? true : false;

      var extractedErr = { writeError: null, wcError: null };

      if (err == 'norepl' || err == 'noreplset') {
        // Know this is legacy gle and the repl not enforced - write concern error in 2.4.
        var errObj = { code: WRITE_CONCERN_FAILED };

        if (errMsg != '') {
          errObj.errmsg = errMsg;
        }
        else if (wNote != '') {
          errObj.errmsg = wNote;
        }
        else {
          errObj.errmsg = err;
        }

        extractedErr.wcError = errObj;
      }
      else if (timeout) {
        // Know there was not write error.
        var errObj = { code: WRITE_CONCERN_FAILED };

        if (errMsg != '') {
          errObj.errmsg = errMsg;
        }
        else {
          errObj.errmsg = err;
        }

        errObj.errInfo = { wtimeout: true };
        extractedErr.wcError = errObj;
      }
      else if (code == 19900 || // No longer primary
               code == 16805 || // replicatedToNum no longer primary
               code == 14330 || // gle wmode changed; invalid
               code == NOT_MASTER ||
               code == UNKNOWN_REPL_WRITE_CONCERN ||
               code == WRITE_CONCERN_FAILED) {
        extractedErr.wcError = {
          code: code,
          errmsg: errMsg
        };
      }
      else if (!isOK) {
        throw Error('Unexpected error from getLastError: ' + tojson(gleResponse));
      }
      else if (err != '') {
        extractedErr.writeError = {
          code: (code == 0)? UNKNOWN_ERROR : code,
          errmsg: err
        };
      }
      else if (jNote != '') {
        extractedErr.writeError = {
          code: WRITE_CONCERN_FAILED,
          errmsg: jNote
        };
      }

      // Handling of writeback not needed for mongo shell.
      return extractedErr;
    };

    // Execute the operations, serially
    var executeBatchWithLegacyOps = function(batch) {

      var batchResult = {
          n: 0
        , writeErrors: []
        , upserted: []
      };

      var extractedError = null;

      var totalToExecute = batch.operations.length;
      // Run over all the operations
      for(var i = 0; i < batch.operations.length; i++) {

        if(batchResult.writeErrors.length > 0 && ordered) break;

        var _legacyOp = new LegacyOp(batch.batchType, batch.operations[i], i);
        executeLegacyOp(_legacyOp);

        var result = executeGetLastError(collection.getDB(), { w: 1 });
        extractedError = extractGLEErrors(result);

        if (extractedError.writeError != null) {
          // Create the emulated result set
          var errResult = {
              index: _legacyOp.index
            , code: extractedError.writeError.code
            , errmsg: extractedError.writeError.errmsg
            , op: batch.operations[_legacyOp.index]
          };

          batchResult.writeErrors.push(errResult);
        }
        else if(_legacyOp.batchType == INSERT) {
          // Inserts don't give us "n" back, so we can only infer
          batchResult.n = batchResult.n + 1;
        }

        if(_legacyOp.batchType == UPDATE) {
          if(result.upserted) {
            batchResult.n = batchResult.n + 1;
            batchResult.upserted.push({
                index: _legacyOp.index
              , _id: result.upserted
            });
          } else if(result.n) {
            batchResult.n = batchResult.n + result.n;
          }
        }

        if(_legacyOp.batchType == REMOVE && result.n) {
          batchResult.n = batchResult.n + result.n;
        }
      }

      var needToEnforceWC = writeConcern != null &&
            bsonWoCompare(writeConcern, { w: 1 }) != 0 &&
            bsonWoCompare(writeConcern, { w: 0 }) != 0;

      if (needToEnforceWC &&
            (batchResult.writeErrors.length == 0 ||
              (!ordered &&
               // not all errored.
               batchResult.writeErrors.length < batch.operations.length))) {

          // if last write errored
          if( batchResult.writeErrors.length > 0 &&
              batchResult.writeErrors[batchResult.writeErrors.length - 1].index ==
              (batch.operations.length - 1)) {
              // Reset previous errors so we can apply the write concern no matter what
              // as long as it is valid.
              collection.getDB().runCommand({ resetError: 1 });
          }

          result = executeGetLastError(collection.getDB(), writeConcern);
          extractedError = extractGLEErrors(result);
      }

      if (extractedError != null && extractedError.wcError != null) {
        bulkResult.writeConcernErrors.push(extractedError.wcError);
      }

      // Merge the results
      mergeBatchResults(batch, bulkResult, batchResult);
    }

    //
    // Execute the batch
    this.execute = function(_writeConcern) {
      if(executed) throw Error("A bulk operation cannot be re-executed");

      // If writeConcern set, use it, else get from collection (which will inherit from db/mongo)
      writeConcern = _writeConcern ? _writeConcern : coll.getWriteConcern();
      if (writeConcern instanceof WriteConcern)
          writeConcern = writeConcern.toJSON();

      // If we have current batch
      if(currentBatch) batches.push(currentBatch);

      // Total number of batches to execute
      var totalNumberToExecute = batches.length;

      var useWriteCommands = collection.getMongo().useWriteCommands();

      // Execute all the batches
      for(var i = 0; i < batches.length; i++) {

        // Execute the batch
        if(collection.getMongo().hasWriteCommands() && 
           collection.getMongo().writeMode() == "commands") {
          executeBatch(batches[i]);
        } else {
          executeBatchWithLegacyOps(batches[i]);
        }

        // If we are ordered and have errors and they are
        // not all replication errors terminate the operation
        if(bulkResult.writeErrors.length > 0 && ordered) {
          // Ordered batches can't enforce full-batch write concern if they fail - they fail-fast
          bulkResult.writeConcernErrors = [];
          break;
        }
      }

      // Set as executed
      executed = true;

      if(batches.length == 1) {
        return new BulkWriteResult(bulkResult, batches[0], writeConcern);
      }

      // Execute the batch and return the final results
      return new BulkWriteResult(bulkResult, null, writeConcern);
    }
  }
})();

if ( ( typeof WriteConcern ) == 'undefined' ){

    /**
     * Shell representation of WriteConcern, possibly includes:
     *  j: write waits for journal
     *  w: write waits for replicated to number of servers (including primary), or mode (string)
     *  wtimeout: how long to wait for "w" replication
     *  fsync: waits for data flush (either journal, nor database files depending on server conf)
     *
     * Accepts { w : x, j : x, wtimeout : x, fsync: x } or w, wtimeout, j
     */
    WriteConcern = function(wValue, wTimeout, jValue) {

        var opts = {};
        if (typeof wValue == 'object') {
            if (typeof jValue == 'undefined' && typeof wTimeout == 'undefined')
                opts = Object.merge(wValue);
            else
                throw Error("If the first arg is an Object then no additional args are allowed!")
        } else {
          if (typeof wValue != 'undefined')
            opts.w = wValue;
          if (typeof wTimeout != 'undefined')
            opts.wtimeout = wTimeout;
          if (typeof jValue != 'undefined')
            opts.j = jValue;
        }

        // Do basic validation.
        if (typeof opts.w != 'undefined' && typeof opts.w != 'number' && typeof opts.w != 'string')
            throw Error("w value must be a number or string but was found to be a " + typeof opts.w)
        if (typeof opts.w == 'number' && NumberInt( opts.w ).toNumber() < 0)
            throw Error("Numeric w value must be equal to or larger than 0, not " + opts.w);

        if (typeof opts.wtimeout != 'undefined') {
            if (typeof opts.wtimeout != 'number')
                throw Error("wtimeout must be a number, not " + opts.wtimeout);
            if (NumberInt( opts.wtimeout ).toNumber() < 0)
                throw Error("wtimeout must be a number greater than 0, not " + opts.wtimeout);
        }
        
        if (typeof opts.j != 'undefined' && typeof opts.j != 'boolean')
            throw Error("j value must either true or false if defined, not " + opts.j);
        
        this._wc = opts;
    };

    /**
     * @return {object} the object representation of this object. Use tojson (small caps) to get
     *     the string representation instead.
     */
    WriteConcern.prototype.toJSON = function() {
        return Object.merge({}, this._wc);
    };

    /**
     * @return {string} the string representation of this object. Use toJSON (capitalized) to get
     *     the object representation instead.
     */
    WriteConcern.prototype.tojson = function(indent, nolint) {
        return tojson(this.toJSON(), indent, nolint);
    };

    WriteConcern.prototype.toString = function() {
        return "WriteConcern(" + this.tojson() + ")";
    };

    WriteConcern.prototype.shellPrint = function() {
        return this.toString();
    };
}
