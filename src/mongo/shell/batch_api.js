//
// Scope for the function
//
var _batch_api_module = (function() {
  // Insert types
  var NONE = 0;
  var INSERT = 1;
  var UPDATE = 2;
  var REMOVE = 3

  // Error codes
  var UNKNOWN_ERROR = 8;
  var WRITE_CONCERN_ERROR = 64;

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
    if(options.w) cmd.w = options.w;
    if(options.wtimeout) cmd.wtimeout = options.wtimeout;
    if(options.j) cmd.j = options.j;
    if(options.fsync) cmd.fsync = options.fsync;

    // Execute the getLastErrorCommand
    var res = db.runCommand( cmd );

    if(res.ok == 0)
        throw "getlasterror failed: " + tojson( res );
    return res;
  }

  /**
   * Wraps the result for the commands
   */
  var BulkWriteResult = function(bulkResult) {
    // Define properties
    defineReadOnlyProperty(this, "ok", bulkResult.ok);
    defineReadOnlyProperty(this, "nInserted", bulkResult.nInserted);
    defineReadOnlyProperty(this, "nUpserted", bulkResult.nUpserted);
    defineReadOnlyProperty(this, "nUpdated", bulkResult.nUpdated);
    defineReadOnlyProperty(this, "nModified", bulkResult.nUpserted);
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
        bulkResult.writeConcernErrors[0];
      } else {

        // Combine the errors
        var errmsg = "";
        for(var i = 0; i < bulkResult.writeConcernErrors.length; i++) {
          var err = bulkResult.writeConcernErrors[i];
          errmsg = errmsg + err.errmsg;
          // TODO: Something better
          if(i == 0) errmsg = errmsg + " and ";
        }

        return new WriteConcernError({ errmsg : errmsg, code : WRITE_CONCERN_ERROR });
      }
    }

    this.tojson = function() {
      return bulkResult;
    }

    this.toString = function() {
      return "BulkWriteResult(" + tojson(bulkResult) + ")";
    }

    this.shellPrint = function() {
      return this.toString();
    }

    this.isOK = function() {
      return bulkResult.ok == 1;
    }
  }

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

    this.tojson = function() {
      return err;
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
    defineReadOnlyProperty(this, "errmsg", err.errmsg);

    this.tojson = function() {
      return err;
    }

    this.toString = function() {
      return "WriteConcernError(" + err.errmsg + ")";
    }

    this.shellPrint = function() {
      return this.toString();
    }
  }

  /**
   * Keeps the state of a unordered batch so we can rewrite the results
   * correctly after command execution
   */
  var Batch = function(batchType, originalZeroIndex) {
    this.originalZeroIndex = originalZeroIndex;
    this.batchType = batchType;
    this.operations = [];
    this.size = 0;
  }

  /**
   * Wraps a legacy operation so we can correctly rewrite it's error
   */
  var LegacyOp = function(batchType, operation, index) {
    this.batchType = batchType;
    this.index = index;
    this.operation = operation;
  }

  /***********************************************************
   * Adds the initializers of bulk operations to the db collection
   ***********************************************************/
  DBCollection.prototype.initializeUnorderedBulkOp = function(options) {
    return new Bulk(this, false, options)
  }

  DBCollection.prototype.initializeOrderedBulkOp = function(options) {
    return new Bulk(this, true, options)
  }

  /***********************************************************
   * Wraps the operations done for the batch
   ***********************************************************/
  var Bulk = function(collection, ordered, options) {
    options = options == null ? {} : options;

    // Namespace for the operation
    var self = this;
    var namespace = collection.getName();
    var maxTimeMS = options.maxTimeMS;
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
      , nUpdated: 0
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

    // Add to internal list of documents
    var addToOperationsList = function(docType, document) {
      // Get the bsonSize
      var bsonSize = Object.bsonsize(document);
      // Create a new batch object if we don't have a current one
      if(currentBatch == null) currentBatch = new Batch(docType, currentIndex);

      // Update current batch size
      currentBatchSize = currentBatchSize + 1;
      currentBatchSizeBytes = currentBatchSizeBytes + bsonSize;

      // Check if we need to create a new batch
      if((currentBatchSize >= maxNumberOfDocsInBatch)
        || (currentBatchSizeBytes >= maxBatchSizeBytes)
        || (currentBatch.batchType != docType)) {
        // Save the batch to the execution stack
        batches.push(currentBatch);

        // Create a new batch
        currentBatch = new Batch(docType, currentIndex);

        // Reset the current size trackers
        currentBatchSize = 0;
        currentBatchSizeBytes = 0;
      }

      // We have an array of documents
      if(Array.isArray(document)) {
        throw new "operation passed in cannot be an Array";
      } else {
        currentBatch.operations.push(document)
        currentIndex = currentIndex + 1;
      }
    }

    // Add the insert document
    this.insert = function(document) {
      return addToOperationsList(INSERT, document);
    }

    //
    // Find based operations
    var findOperations = {
      update: function(updateDocument) {
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
        // Establish the update command
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
        // Establish the update command
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
        throw "legacy batch failed, cannot aggregate results: " + result.writeConcernError.errmsg;
      }

      // If we have an insert Batch type
      if(batch.batchType == INSERT) {
        bulkResult.nInserted = bulkResult.nInserted + result.n;
      }

      // If we have an insert Batch type
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
        var nModified = result.nDocsModified ? result.nDocsModified : 0;
        bulkResult.nUpserted = bulkResult.nUpserted + nUpserted;
        bulkResult.nUpdated = bulkResult.nUpdated + (result.n - nUpserted);
        bulkResult.nModified = bulkResult.nModified + nModified;
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
        cmd = { update: namespace, updates: batch.operations, ordered: ordered }
      } else if(batch.batchType == INSERT) {
        cmd = { insert: namespace, documents: batch.operations, ordered: ordered }
      } else if(batch.batchType == REMOVE) {
        cmd = { delete: namespace, deletes: batch.operations, ordered: ordered }
      }

      // If we have a write concern
      if(writeConcern != null) {
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
        throw "batch failed, cannot aggregate results: " + result.errmsg;
      }

      // Merge the results
      mergeBatchResults(batch, bulkResult, result);
    }

    // Execute a single legacy op
    var executeLegacyOp = function(_legacyOp) {
      // Handle the different types of operation types
      if(_legacyOp.batchType == INSERT) {
        collection.insert(_legacyOp.operation);
      } else if(_legacyOp.batchType == UPDATE) {
        if(_legacyOp.operation.multi) options.multi = _legacyOp.operation.multi;
        if(_legacyOp.operation.upsert) options.upsert = _legacyOp.operation.upsert;

        collection.update(_legacyOp.operation.q
          , _legacyOp.operation.u, options.upsert, options.update);
      } else if(_legacyOp.batchType == REMOVE) {
        if(_legacyOp.operation.limit) options.single = true;

        collection.remove(_legacyOp.operation.q, options.single);
      }

      // Retrieve the lastError object
      return executeGetLastError(collection.getDB(), writeConcern);
    }

    // Execute the operations, serially
    var executeBatchWithLegacyOps = function(batch) {

      var batchResult = {
          n: 0
        , nDocsModified: 0
        , writeErrors: []
        , upserted: []
      };

      var totalToExecute = batch.operations.length;
      // Run over all the operations
      for(var i = 0; i < batch.operations.length; i++) {

        if(batchResult.writeErrors.length > 0 && ordered) break;

        var _legacyOp = new LegacyOp(batch.batchType, batch.operations[i], i);
        var result = executeLegacyOp(_legacyOp);

        // Result is replication issue, rewrite error to match write command
        if(result.wnote || result.wtimeout || result.jnote) {

          // If we are ordered and have a jnote or wnote throw to be compatible
          // with write command behavior on pre 2.6
          if(ordered && (result.jnote || result.wnote || result.wtimeout)) {
            throw "legacy batch failed, cannot aggregate results: " + result.errmsg;
          }

          // Sometimes, with replication, we get an errmsg *and* wnote/jnote/wtimeout
          // Ensure we get the right error message
          errmsg = result.wnote || errmsg;
          errmsg = result.jnote || errmsg;

          bulkResult.writeConcernErrors.push({ errmsg: errmsg, code: WRITE_CONCERN_ERROR });
        } else {
          if(result.ok == 0) {
            throw "legacy batch failed, cannot aggregate results: " + result.errmsg;
          }
        }

        // Handle error (it's only a write error if there is no wnote/jnote or wtimeout)
        if(result.err != null
          && (result.wnote == null && result.jnote == null && result.wtimeout == null)) {
          var code = result.code || UNKNOWN_ERROR; // Returned error code or unknown code
          var errmsg = result.errmsg || result.err;

          // Create the emulated result set
          var errResult = {
              index: _legacyOp.index
            , code: code
            , errmsg: errmsg
            , op: batch.operations[_legacyOp.index]
          };

          batchResult.writeErrors.push(errResult);

        } else if(_legacyOp.batchType == INSERT) {
          // Inserts don't give us "n" back, so we can only infer
          if(result.code == null)
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
            batchResult.nDocsModified = batchResult.nDocsModified + result.n;
          }
        }

        if(_legacyOp.batchType == REMOVE && result.n) {
          batchResult.n = batchResult.n + result.n;
        }
      }

      // Merge the results
      mergeBatchResults(batch, bulkResult, batchResult);
    }

    //
    // Execute the batch
    this.execute = function(_writeConcern) {
      if(executed) throw "batch cannot be re-executed";

      // If writeConcern set
      if(_writeConcern) writeConcern = _writeConcern;

      // If we have current batch
      if(currentBatch) batches.push(currentBatch);

      // Total number of batches to execute
      var totalNumberToExecute = batches.length;

      var useWriteCommands = collection.getMongo().useWriteCommands();

      // Execute all the batches
      for(var i = 0; i < batches.length; i++) {

        // Execute the batch
        if(useWriteCommands) {
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

      // Execute the batch and return the final results
      executed = true;
      return new BulkWriteResult(bulkResult);
    }
  }
})()