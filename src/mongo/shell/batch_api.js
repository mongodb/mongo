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
  var MULTIPLE_ERROR = 65;

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
  var BatchWriteResult = function(batchResult) {
    // Define properties
    defineReadOnlyProperty(this, "ok", batchResult.ok);
    defineReadOnlyProperty(this, "n", batchResult.n);
    defineReadOnlyProperty(this, "nInserted", batchResult.nInserted);
    defineReadOnlyProperty(this, "nUpdated", batchResult.nUpdated);
    defineReadOnlyProperty(this, "nUpserted", batchResult.nUpserted);
    defineReadOnlyProperty(this, "nRemoved", batchResult.nRemoved);
    
    //
    // Define access methods
    this.getUpsertedIds = function() {
      return batchResult.upserted;
    }

    this.getUpsertedIdAt = function(index) {
      return batchResult.upserted[index]; 
    }

    this.getRawResponse = function() {
      return batchResult;
    }

    this.getSingleError = function() {
      if(this.hasErrors()) {
        return new WriteError({
            code: MULTIPLE_ERROR
          , errmsg: "batch item errors occurred"
          , index: 0
        })
      }
    }

    this.hasErrors = function() {
      return batchResult.errDetails.length > 0;
    }

    this.getErrorCount = function() {
      var count = 0;
      if(batchResult.errDetails) {
        count = count + batchResult.errDetails.length;
      } else if(batchResult.ok == 0) {
        count = count + 1;
      }

      return count;
    }

    this.getErrorAt = function(index) {
      if(batchResult.errDetails 
        && index < batchResult.errDetails.length) {
        return new WriteError(batchResult.errDetails[index]);
      }

      return null;
    }

    //
    // Get all errors
    this.getErrors = function() {
      return batchResult.errDetails;
    }

    //
    // Return all non wc errors
    this.getWriteErrors = function() {
      var errors = [];

      // No errors, return empty list
      if(!Array.isArray(batchResult.errDetails)) return errors;

      // Locate any non WC errors
      for(var i = 0; i < batchResult.errDetails.length; i++) {
        // 64 signals a write concern error
        if(batchResult.errDetails[i].code != WRITE_CONCERN_ERROR) {
          errors.push(new WriteError(batchResult.errDetails[i]));
        }
      }

      // Return the errors
      return errors;
    }

    this.getWCErrors = function() {
      var wcErrors = [];
      // No errDetails return no WCErrors
      if(!Array.isArray(batchResult.errDetails)) return wcErrors;

      // Locate any WC errors
      for(var i = 0; i < batchResult.errDetails.length; i++) {
        // 64 signals a write concern error
        if(batchResult.errDetails[i].code == WRITE_CONCERN_ERROR) {
          wcErrors.push(new WriteError(batchResult.errDetails[i]));
        }
      }

      // Return the errors
      return wcErrors;
    }

    this.tojson = function() {
      return batchResult;
    }

    this.toString = function() {
      return "BatchWriteResult(" + tojson(batchResult) + ")";
    }

    this.shellPrint = function() {
      return this.toString();
    }

    this.isOK = function() {
      return batchResult.ok == 1;
    }
  }

  /**
   * Wraps the error
   */
  var WriteError = function(err) {
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
    var mergeResults = { 
        n: 0
      , upserted: []
      , errDetails: []
      , wcErrors: 0
      , nInserted: 0
      , nUpserted: 0
      , nUpdated: 0
      , nRemoved: 0      
    }

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
    var mergeBatchResults = function(batch, mergeResult, result) {
      // Get the n
      var n = typeof result.n != 'number' ? 0 : result.n;
      // Add the results
      mergeResult.n = mergeResult.n + n;
    
      // If we have an insert Batch type
      if(batch.batchType == INSERT) {
        mergeResult.nInserted = mergeResult.nInserted + result.n;
      }

      // If we have an insert Batch type
      if(batch.batchType == REMOVE) {
        mergeResult.nRemoved = mergeResult.nRemoved + result.n;
      }

      // We have an array of upserted values, we need to rewrite the indexes
      if(Array.isArray(result.upserted)) {
        mergeResult.nUpserted = mergeResult.nUpserted + result.upserted.length;
        mergeResult.nUpdated = mergeResult.nUpdated + (result.n - result.upserted.length);

        for(var i = 0; i < result.upserted.length; i++) {
          mergeResult.upserted.push({
              index: result.upserted[i].index + batch.originalZeroIndex
            , _id: result.upserted[i]._id
          });
        }
      } else if(result.upserted) { 
        mergeResult.nUpserted = mergeResult.nUpserted + 1;
        mergeResult.nUpdated = mergeResult.nUpdated + (result.n - 1);
        mergeResult.upserted.push({
            index: batch.originalZeroIndex
          , _id: result.upserted
        });           
      }

      // We have a top level error as well as single operation errors
      // in errDetails, apply top level and override with errDetails ones
      if(result.ok == 0) {
        // Error details
        var errDetails = [];
        var numberOfOperations = batch.operations.length;

        // Establish if we need to cut off top level errors due to ordered
        if(ordered && Array.isArray(result.errDetails)) {
          numberOfOperations = result.errDetails[result.errDetails.length - 1].index;
        }

        // Apply any errDetails      
        if(Array.isArray(result.errDetails)) {
          for(var i = 0; i < result.errDetails.length; i++) {
            var index = result.code != MULTIPLE_ERROR ? result.errDetails[i].index : i;

            // Update the number of replication errors
            if(result.errDetails[i].code == WRITE_CONCERN_ERROR) {
              mergeResult.wcErrors = mergeResult.wcErrors + 1;
            }

            errDetails[index] = {
                index: batch.originalZeroIndex + result.errDetails[i].index
              , code: result.errDetails[i].code
              , errmsg: result.errDetails[i].errmsg
              , op: batch.operations[result.errDetails[i].index]
            }
          }          
        }

        // Any other errors get the batch error code, if one exists
        if(result.code != MULTIPLE_ERROR) {
        
          // All errors without errDetails are affected by the batch error
          for(var i = 0; i < numberOfOperations; i++) {
          
            if(errDetails[i]) continue;
          
            // Update the number of replication errors
            if(result.code == WRITE_CONCERN_ERROR) {
              mergeResult.wcErrors = mergeResult.wcErrors + 1;
            }

            // Add the error to the errDetails
            errDetails[i] = {
                index: batch.originalZeroIndex + i
              , code: result.code
              , errmsg: result.errmsg
              , op: batch.operations[i]           
            };
          }
        }


        // Merge the error details
        mergeResults.errDetails = mergeResults.errDetails.concat(errDetails);
        return;
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

      // Run the command
      try {
        // Get command collection
        var cmdColl = collection._db.getCollection('$cmd');        
        // Bypass runCommand to ignore slaveOk and read pref settings
        result = new DBQuery(collection._mongo, collection._db, cmdColl, cmdColl.getFullName(), cmd,
                        {} /* proj */, -1 /* limit */, 0 /* skip */, 0 /* batchSize */,
                        0 /* flags */).next();
      } catch(err) {
        // Create a top level error
        result = { 
            ok: 0
          , code : err.code ? err.code : UNKNOWN_ERROR
          , errmsg : err.message ? err.message : err          
        };

        // Add errInfo if available
        if(err.errInfo) result.errInfo = err.errInfo;
      }

      // Merge the results
      mergeBatchResults(batch, mergeResults, result);
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
      return executeGetLastError(collection._db, writeConcern);
    }

    // Execute the operations, serially
    var executeBatchWithLegacyOps = function(_mergeResults, batch) {
      var totalToExecute = batch.operations.length;
      // Run over all the operations
      for(var i = 0; i < batch.operations.length; i++) {
        var _legacyOp = new LegacyOp(batch.batchType, batch.operations[i], i);
        var result = executeLegacyOp(_legacyOp);

        // Handle error
        if(result.errmsg || result.err) {
          var code = result.code || UNKNOWN_ERROR; // Returned error code or unknown code
          var errmsg = result.errmsg || result.err;

          // Result is replication issue, rewrite error to match write command      
          if(result.wnote || result.wtimeout || result.jnote) {
            // Update the replication counters
            _mergeResults.n = _mergeResults.n + 1;
            _mergeResults.wcErrors = _mergeResults.wcErrors + 1;
            // Set the code to replication error
            code = WRITE_CONCERN_ERROR;
            // Ensure we get the right error message
            errmsg = result.wnote || errmsg;
            errmsg = result.jnote || errmsg;
          }

          // Create the emulated result set
          var errResult = {
              index: _legacyOp.index + batch.originalZeroIndex
            , code: code
            , errmsg: errmsg
            , op: batch.operations[_legacyOp.index]
          };

          if(result.errInfo) errResult.errInfo = result.errInfo;
            _mergeResults.errDetails.push(errResult);

          // Check if we any errors
          if(ordered == true 
            && result.jnote == null 
            && result.wnote == null 
            && result.wtimeout == null) {
            return new BatchWriteResult(_mergeResults);
          }
        } else if(_legacyOp.batchType == INSERT) {
          _mergeResults.n = _mergeResults.n + 1;
          _mergeResults.nInserted = _mergeResults.nInserted + 1;
        } else if(_legacyOp.batchType == UPDATE) {
          _mergeResults.n = _mergeResults.n + result.n;
          if(result.upserted) {
            _mergeResults.nUpserted = _mergeResults.nUpserted + 1;
          } else {
            _mergeResults.nUpdated = _mergeResults.nUpdated + 1;
          }
        } else if(_legacyOp.batchType == REMOVE) {
          _mergeResults.n = _mergeResults.n + result.n;
          _mergeResults.nRemoved = _mergeResults.nRemoved + result.n;
        }

        // We have an upserted field (might happen with a write concern error)
        if(result.upserted) _mergeResults.upserted.push({
            index: _legacyOp.index + batch.originalZeroIndex
          , _id: result.upserted
        })        
      }

      // Return the aggregated results
      return new BatchWriteResult(_mergeResults);
    }

    //
    // Execute the unordered batch
    this.execute = function(_writeConcern) {
      if(executed) throw "batch cannot be re-executed";

      // If writeConcern set
      if(_writeConcern) writeConcern = _writeConcern;

      // If we have current batch
      if(currentBatch) batches.push(currentBatch);
      
      // Total number of batches to execute
      var totalNumberToExecute = batches.length;

      // We can use write commands
      if(collection._mongo.useWriteCommands()) {
        // Execute all the batches
        for(var i = 0; i < batches.length; i++) {
          // Execute the batch
          executeBatch(batches[i]);
          
          // If we are ordered and have errors and they are 
          // not all replication errors terminate the operation          
          if(mergeResults.errDetails.length > 0 
            && mergeResults.errDetails.length != mergeResults.wcErrors 
            && ordered) {
              return new BatchWriteResult(mergeResults);            
          }
        }

        // Return the aggregated final result
        return new BatchWriteResult(mergeResults);
      }

      // Execute in backwards compatible mode
      for(var i = 0; i < batches.length; i++) {
        executeBatchWithLegacyOps(mergeResults, batches[i]);
        // If we have an ordered batch finish up processing on error
        if(mergeResults.errDetails.length > 0 
          && mergeResults.errDetails.length != mergeResults.wcErrors
          && ordered) {
          break;
        }
      }

      // Execute the batch and return the final results
      executed = true;
      return new BatchWriteResult(mergeResults);
    }   
  } 
})()