DBCollection.prototype._createWriteConcern = function(options) {
    // If writeConcern set, use it, else get from collection (which will inherit from db/mongo)
    var writeConcern = options.writeConcern || this.getWriteConcern();
    var writeConcernOptions = ['w', 'wtimeout', 'j', 'fsync'];

    if (writeConcern instanceof WriteConcern) {
        writeConcern = writeConcern.toJSON();
    }

    // Only merge in write concern options if at least one is specified in options
    if (options.w != null || options.wtimeout != null || options.j != null ||
        options.fsync != null) {
        writeConcern = {};

        writeConcernOptions.forEach(function(wc) {
            if (options[wc] != null) {
                writeConcern[wc] = options[wc];
            }
        });
    }

    return writeConcern;
};

/**
 * @return {Object} a new document with an _id: ObjectId if _id is not present.
 *     Otherwise, returns the same object passed.
 */
DBCollection.prototype.addIdIfNeeded = function(obj) {
    if (typeof(obj._id) == "undefined" && !Array.isArray(obj)) {
        var tmp = obj;  // don't want to modify input
        obj = {_id: new ObjectId()};

        for (var key in tmp) {
            obj[key] = tmp[key];
        }
    }

    return obj;
};

/**
* Perform a bulkWrite operation without a fluent API
*
* Legal operation types are
*
*  { insertOne: { document: { a: 1 } } }
*
*  { updateOne: { filter: {a:2}, update: {$set: {a:2}}, upsert:true, collation: {locale: "fr"} } }
*
*  { updateMany: { filter: {a:2}, update: {$set: {a:2}}, upsert:true collation: {locale: "fr"} } }
*
*  { deleteOne: { filter: {c:1}, collation: {locale: "fr"} } }
*
*  { deleteMany: { filter: {c:1}, collation: {locale: "fr"} } }
*
*  { replaceOne: { filter: {c:3}, replacement: {c:4}, upsert:true, collation: {locale: "fr"} } }
*
* @method
* @param {object[]} operations Bulk operations to perform.
* @param {object} [options=null] Optional settings.
* @param {(number|string)} [options.w=null] The write concern.
* @param {number} [options.wtimeout=null] The write concern timeout.
* @param {boolean} [options.j=false] Specify a journal write concern.
* @return {object}
*/
DBCollection.prototype.bulkWrite = function(operations, options) {
    var opts = Object.extend({}, options || {});
    opts.ordered = (typeof opts.ordered == 'boolean') ? opts.ordered : true;

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Result
    var result = {acknowledged: (writeConcern && writeConcern.w == 0) ? false : true};

    // Use bulk operation API already in the shell
    var bulkOp = opts.ordered ? this.initializeOrderedBulkOp() : this.initializeUnorderedBulkOp();

    // Contains all inserted _ids
    var insertedIds = {};

    // For each of the operations we need to add the op to the bulk
    operations.forEach(function(op, index) {
        if (op.insertOne) {
            if (!op.insertOne.document) {
                throw new Error('insertOne bulkWrite operation expects the document field');
            }

            // Add _id ObjectId if needed
            op.insertOne.document = this.addIdIfNeeded(op.insertOne.document);
            // InsertedIds is a map of [originalInsertOrderIndex] = document._id
            insertedIds[index] = op.insertOne.document._id;
            // Translate operation to bulk operation
            bulkOp.insert(op.insertOne.document);
        } else if (op.updateOne) {
            if (!op.updateOne.filter) {
                throw new Error('updateOne bulkWrite operation expects the filter field');
            }

            if (!op.updateOne.update) {
                throw new Error('updateOne bulkWrite operation expects the update field');
            }

            // Translate operation to bulk operation
            var operation = bulkOp.find(op.updateOne.filter);
            if (op.updateOne.upsert) {
                operation = operation.upsert();
            }

            if (op.updateOne.collation) {
                operation.collation(op.updateOne.collation);
            }

            operation.updateOne(op.updateOne.update);
        } else if (op.updateMany) {
            if (!op.updateMany.filter) {
                throw new Error('updateMany bulkWrite operation expects the filter field');
            }

            if (!op.updateMany.update) {
                throw new Error('updateMany bulkWrite operation expects the update field');
            }

            // Translate operation to bulk operation
            var operation = bulkOp.find(op.updateMany.filter);
            if (op.updateMany.upsert) {
                operation = operation.upsert();
            }

            if (op.updateMany.collation) {
                operation.collation(op.updateMany.collation);
            }

            operation.update(op.updateMany.update);
        } else if (op.replaceOne) {
            if (!op.replaceOne.filter) {
                throw new Error('replaceOne bulkWrite operation expects the filter field');
            }

            if (!op.replaceOne.replacement) {
                throw new Error('replaceOne bulkWrite operation expects the replacement field');
            }

            // Translate operation to bulkOp operation
            var operation = bulkOp.find(op.replaceOne.filter);
            if (op.replaceOne.upsert) {
                operation = operation.upsert();
            }

            if (op.replaceOne.collation) {
                operation.collation(op.replaceOne.collation);
            }

            operation.replaceOne(op.replaceOne.replacement);
        } else if (op.deleteOne) {
            if (!op.deleteOne.filter) {
                throw new Error('deleteOne bulkWrite operation expects the filter field');
            }

            // Translate operation to bulkOp operation.
            var deleteOp = bulkOp.find(op.deleteOne.filter);

            if (op.deleteOne.collation) {
                deleteOp.collation(op.deleteOne.collation);
            }

            deleteOp.removeOne();
        } else if (op.deleteMany) {
            if (!op.deleteMany.filter) {
                throw new Error('deleteMany bulkWrite operation expects the filter field');
            }

            // Translate operation to bulkOp operation.
            var deleteOp = bulkOp.find(op.deleteMany.filter);

            if (op.deleteMany.collation) {
                deleteOp.collation(op.deleteMany.collation);
            }

            deleteOp.remove();
        }
    }, this);

    // Execute bulkOp operation
    var response = bulkOp.execute(writeConcern);
    if (!result.acknowledged) {
        return result;
    }

    result.deletedCount = response.nRemoved;
    result.insertedCount = response.nInserted;
    result.matchedCount = response.nMatched;
    result.upsertedCount = response.nUpserted;
    result.insertedIds = insertedIds;
    result.upsertedIds = {};

    // Iterate over all the upserts
    var upserts = response.getUpsertedIds();
    upserts.forEach(function(x) {
        result.upsertedIds[x.index] = x._id;
    });

    // Return the result
    return result;
};

/**
* Inserts a single document into MongoDB.
*
* @method
* @param {object} doc Document to insert.
* @param {object} [options=null] Optional settings.
* @param {(number|string)} [options.w=null] The write concern.
* @param {number} [options.wtimeout=null] The write concern timeout.
* @param {boolean} [options.j=false] Specify a journal write concern.
* @return {object}
*/
DBCollection.prototype.insertOne = function(document, options) {
    var opts = Object.extend({}, options || {});

    // Add _id ObjectId if needed
    document = this.addIdIfNeeded(document);

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Result
    var result = {acknowledged: (writeConcern && writeConcern.w == 0) ? false : true};

    // Use bulk operation API already in the shell
    var bulk = this.initializeOrderedBulkOp();
    bulk.insert(document);

    try {
        // Execute insert
        bulk.execute(writeConcern);
    } catch (err) {
        if (err.hasWriteErrors()) {
            throw err.getWriteErrorAt(0);
        }

        if (err.hasWriteConcernError()) {
            throw err.getWriteConcernError();
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    // Set the inserted id
    result.insertedId = document._id;

    // Return the result
    return result;
};

/**
* Inserts an array of documents into MongoDB.
*
* @method
* @param {object[]} docs Documents to insert.
* @param {object} [options=null] Optional settings.
* @param {(number|string)} [options.w=null] The write concern.
* @param {number} [options.wtimeout=null] The write concern timeout.
* @param {boolean} [options.j=false] Specify a journal write concern.
* @param {boolean} [options.ordered=true] Execute inserts in ordered or unordered fashion.
* @return {object}
*/
DBCollection.prototype.insertMany = function(documents, options) {
    var opts = Object.extend({}, options || {});
    opts.ordered = (typeof opts.ordered == 'boolean') ? opts.ordered : true;

    // Ensure all documents have an _id
    documents = documents.map(function(x) {
        return this.addIdIfNeeded(x);
    }, this);

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Result
    var result = {acknowledged: (writeConcern && writeConcern.w == 0) ? false : true};

    // Use bulk operation API already in the shell
    var bulk = opts.ordered ? this.initializeOrderedBulkOp() : this.initializeUnorderedBulkOp();

    // Add all operations to the bulk operation
    documents.forEach(function(doc) {
        bulk.insert(doc);
    });

    // Execute bulk write operation
    bulk.execute(writeConcern);

    if (!result.acknowledged) {
        return result;
    }

    // Set all the created inserts
    result.insertedIds = documents.map(function(x) {
        return x._id;
    });

    // Return the result
    return result;
};

/**
* Delete a document on MongoDB
*
* @method
* @param {object} filter The filter used to select the document to remove
* @param {object} [options=null] Optional settings.
* @param {(number|string)} [options.w=null] The write concern.
* @param {number} [options.wtimeout=null] The write concern timeout.
* @param {boolean} [options.j=false] Specify a journal write concern.
* @return {object}
*/
DBCollection.prototype.deleteOne = function(filter, options) {
    var opts = Object.extend({}, options || {});

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Result
    var result = {acknowledged: (writeConcern && writeConcern.w == 0) ? false : true};

    // Use bulk operation API already in the shell
    var bulk = this.initializeOrderedBulkOp();
    var removeOp = bulk.find(filter);

    // Add the collation, if there is one.
    if (opts.collation) {
        removeOp.collation(opts.collation);
    }

    // Add the deleteOne operation.
    removeOp.removeOne();

    try {
        // Remove the first document that matches the selector
        var r = bulk.execute(writeConcern);
    } catch (err) {
        if (err.hasWriteErrors()) {
            throw err.getWriteErrorAt(0);
        }

        if (err.hasWriteConcernError()) {
            throw err.getWriteConcernError();
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    result.deletedCount = r.nRemoved;
    return result;
};

/**
* Delete multiple documents on MongoDB
*
* @method
* @param {object} filter The Filter used to select the documents to remove
* @param {object} [options=null] Optional settings.
* @param {(number|string)} [options.w=null] The write concern.
* @param {number} [options.wtimeout=null] The write concern timeout.
* @param {boolean} [options.j=false] Specify a journal write concern.
* @return {object}
*/
DBCollection.prototype.deleteMany = function(filter, options) {
    var opts = Object.extend({}, options || {});

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Result
    var result = {acknowledged: (writeConcern && writeConcern.w == 0) ? false : true};

    // Use bulk operation API already in the shell
    var bulk = this.initializeOrderedBulkOp();
    var removeOp = bulk.find(filter);

    // Add the collation, if there is one.
    if (opts.collation) {
        removeOp.collation(opts.collation);
    }

    // Add the deleteOne operation.
    removeOp.remove();

    try {
        // Remove all documents that matche the selector
        var r = bulk.execute(writeConcern);
    } catch (err) {
        if (err.hasWriteErrors()) {
            throw err.getWriteErrorAt(0);
        }

        if (err.hasWriteConcernError()) {
            throw err.getWriteConcernError();
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    result.deletedCount = r.nRemoved;
    return result;
};

/**
* Replace a document on MongoDB
*
* @method
* @param {object} filter The Filter used to select the document to update
* @param {object} doc The Document that replaces the matching document
* @param {object} [options=null] Optional settings.
* @param {boolean} [options.upsert=false] Update operation is an upsert.
* @param {(number|string)} [options.w=null] The write concern.
* @param {number} [options.wtimeout=null] The write concern timeout.
* @param {boolean} [options.j=false] Specify a journal write concern.
* @return {object}
*/
DBCollection.prototype.replaceOne = function(filter, replacement, options) {
    var opts = Object.extend({}, options || {});

    // Check if first key in update statement contains a $
    var keys = Object.keys(replacement);
    // Check if first key does not have the $
    if (keys.length > 0 && keys[0][0] == "$") {
        throw new Error('the replace operation document must not contain atomic operators');
    }

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Result
    var result = {acknowledged: (writeConcern && writeConcern.w == 0) ? false : true};

    // Use bulk operation API already in the shell
    var bulk = this.initializeOrderedBulkOp();

    // Add the deleteOne operation
    var op = bulk.find(filter);
    if (opts.upsert) {
        op = op.upsert();
    }

    if (opts.collation) {
        op.collation(opts.collation);
    }

    op.replaceOne(replacement);

    try {
        // Replace the document
        var r = bulk.execute(writeConcern);
    } catch (err) {
        if (err.hasWriteErrors()) {
            throw err.getWriteErrorAt(0);
        }

        if (err.hasWriteConcernError()) {
            throw err.getWriteConcernError();
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    result.matchedCount = r.nMatched;
    result.modifiedCount = (r.nModified != null) ? r.nModified : r.n;

    if (r.getUpsertedIds().length > 0) {
        result.upsertedId = r.getUpsertedIdAt(0)._id;
    }

    return result;
};

/**
* Update a single document on MongoDB
*
* @method
* @param {object} filter The Filter used to select the document to update
* @param {object} update The update operations to be applied to the document
* @param {object} [options=null] Optional settings.
* @param {boolean} [options.upsert=false] Update operation is an upsert.
* @param {(number|string)} [options.w=null] The write concern.
* @param {number} [options.wtimeout=null] The write concern timeout.
* @param {boolean} [options.j=false] Specify a journal write concern.
* @return {object}
*/
DBCollection.prototype.updateOne = function(filter, update, options) {
    var opts = Object.extend({}, options || {});

    // Check if first key in update statement contains a $
    var keys = Object.keys(update);
    if (keys.length == 0) {
        throw new Error("the update operation document must contain at least one atomic operator");
    }

    // Check if first key does not have the $
    if (keys[0][0] != "$") {
        throw new Error('the update operation document must contain atomic operators');
    }

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Result
    var result = {acknowledged: (writeConcern && writeConcern.w == 0) ? false : true};

    // Use bulk operation API already in the shell
    var bulk = this.initializeOrderedBulkOp();

    // Add the updateOne operation
    var op = bulk.find(filter);
    if (opts.upsert) {
        op = op.upsert();
    }

    if (opts.collation) {
        op.collation(opts.collation);
    }

    op.updateOne(update);

    try {
        // Update the first document that matches the selector
        var r = bulk.execute(writeConcern);
    } catch (err) {
        if (err.hasWriteErrors()) {
            throw err.getWriteErrorAt(0);
        }

        if (err.hasWriteConcernError()) {
            throw err.getWriteConcernError();
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    result.matchedCount = r.nMatched;
    result.modifiedCount = (r.nModified != null) ? r.nModified : r.n;

    if (r.getUpsertedIds().length > 0) {
        result.upsertedId = r.getUpsertedIdAt(0)._id;
    }

    return result;
};

/**
* Update multiple documents on MongoDB
*
* @method
* @param {object} filter The Filter used to select the document to update
* @param {object} update The update operations to be applied to the document
* @param {object} [options=null] Optional settings.
* @param {boolean} [options.upsert=false] Update operation is an upsert.
* @param {(number|string)} [options.w=null] The write concern.
* @param {number} [options.wtimeout=null] The write concern timeout.
* @param {boolean} [options.j=false] Specify a journal write concern.
* @return {object}
*/
DBCollection.prototype.updateMany = function(filter, update, options) {
    var opts = Object.extend({}, options || {});

    // Check if first key in update statement contains a $
    var keys = Object.keys(update);
    if (keys.length == 0) {
        throw new Error("the update operation document must contain at least one atomic operator");
    }

    // Check if first key does not have the $
    if (keys[0][0] != "$") {
        throw new Error('the update operation document must contain atomic operators');
    }

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Result
    var result = {acknowledged: (writeConcern && writeConcern.w == 0) ? false : true};

    // Use bulk operation API already in the shell
    var bulk = this.initializeOrderedBulkOp();

    // Add the updateMany operation
    var op = bulk.find(filter);
    if (opts.upsert) {
        op = op.upsert();
    }

    if (opts.collation) {
        op.collation(opts.collation);
    }

    op.update(update);

    try {
        // Update all documents that match the selector
        var r = bulk.execute(writeConcern);
    } catch (err) {
        if (err.hasWriteErrors()) {
            throw err.getWriteErrorAt(0);
        }

        if (err.hasWriteConcernError()) {
            throw err.getWriteConcernError();
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    result.matchedCount = r.nMatched;
    result.modifiedCount = (r.nModified != null) ? r.nModified : r.n;

    if (r.getUpsertedIds().length > 0) {
        result.upsertedId = r.getUpsertedIdAt(0)._id;
    }

    return result;
};

/**
* Find a document and delete it in one atomic operation,
* requires a write lock for the duration of the operation.
*
* @method
* @param {object} filter Document selection filter.
* @param {object} [options=null] Optional settings.
* @param {object} [options.projection=null] Limits the fields to return for all matching documents.
* @param {object} [options.sort=null] Determines which document the operation modifies if the query
*selects multiple documents.
* @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
* @return {object}
*/
DBCollection.prototype.findOneAndDelete = function(filter, options) {
    var opts = Object.extend({}, options || {});
    // Set up the command
    var cmd = {query: filter, remove: true};

    if (opts.sort) {
        cmd.sort = opts.sort;
    }

    if (opts.projection) {
        cmd.fields = opts.projection;
    }

    if (opts.maxTimeMS) {
        cmd.maxTimeMS = opts.maxTimeMS;
    }

    if (opts.collation) {
        cmd.collation = opts.collation;
    }

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Setup the write concern
    if (writeConcern) {
        cmd.writeConcern = writeConcern;
    }

    // Execute findAndModify
    return this.findAndModify(cmd);
};

/**
* Find a document and replace it in one atomic operation, requires a write lock for the duration of
*the operation.
*
* @method
* @param {object} filter Document selection filter.
* @param {object} replacement Document replacing the matching document.
* @param {object} [options=null] Optional settings.
* @param {object} [options.projection=null] Limits the fields to return for all matching documents.
* @param {object} [options.sort=null] Determines which document the operation modifies if the query
*selects multiple documents.
* @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
* @param {boolean} [options.upsert=false] Upsert the document if it does not exist.
* @param {boolean} [options.returnNewDocument=false] When true, returns the updated document rather
*than the original. The default is false.
* @return {object}
*/
DBCollection.prototype.findOneAndReplace = function(filter, replacement, options) {
    var opts = Object.extend({}, options || {});

    // Check if first key in update statement contains a $
    var keys = Object.keys(replacement);
    // Check if first key does not have the $
    if (keys.length > 0 && keys[0][0] == "$") {
        throw new Error("the replace operation document must not contain atomic operators");
    }

    // Set up the command
    var cmd = {query: filter, update: replacement};
    if (opts.sort) {
        cmd.sort = opts.sort;
    }

    if (opts.projection) {
        cmd.fields = opts.projection;
    }

    if (opts.maxTimeMS) {
        cmd.maxTimeMS = opts.maxTimeMS;
    }

    if (opts.collation) {
        cmd.collation = opts.collation;
    }

    // Set flags
    cmd.upsert = (typeof opts.upsert == 'boolean') ? opts.upsert : false;
    cmd.new = (typeof opts.returnNewDocument == 'boolean') ? opts.returnNewDocument : false;

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Setup the write concern
    if (writeConcern) {
        cmd.writeConcern = writeConcern;
    }

    // Execute findAndModify
    return this.findAndModify(cmd);
};

/**
* Find a document and update it in one atomic operation, requires a write lock for the duration of
*the operation.
*
* @method
* @param {object} filter Document selection filter.
* @param {object} update Update operations to be performed on the document
* @param {object} [options=null] Optional settings.
* @param {object} [options.projection=null] Limits the fields to return for all matching documents.
* @param {object} [options.sort=null] Determines which document the operation modifies if the query
*selects multiple documents.
* @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
* @param {boolean} [options.upsert=false] Upsert the document if it does not exist.
* @param {boolean} [options.returnNewDocument=false] When true, returns the updated document rather
*than the original. The default is false.
* @return {object}
*/
DBCollection.prototype.findOneAndUpdate = function(filter, update, options) {
    var opts = Object.extend({}, options || {});

    // Check if first key in update statement contains a $
    var keys = Object.keys(update);
    if (keys.length == 0) {
        throw new Error("the update operation document must contain at least one atomic operator");
    }

    // Check if first key does not have the $
    if (keys[0][0] != "$") {
        throw new Error("the update operation document must contain atomic operators");
    }

    // Set up the command
    var cmd = {query: filter, update: update};
    if (opts.sort) {
        cmd.sort = opts.sort;
    }

    if (opts.projection) {
        cmd.fields = opts.projection;
    }

    if (opts.maxTimeMS) {
        cmd.maxTimeMS = opts.maxTimeMS;
    }

    if (opts.collation) {
        cmd.collation = opts.collation;
    }

    // Set flags
    cmd.upsert = (typeof opts.upsert == 'boolean') ? opts.upsert : false;
    cmd.new = (typeof opts.returnNewDocument == 'boolean') ? opts.returnNewDocument : false;

    // Get the write concern
    var writeConcern = this._createWriteConcern(opts);

    // Setup the write concern
    if (writeConcern) {
        cmd.writeConcern = writeConcern;
    }

    // Execute findAndModify
    return this.findAndModify(cmd);
};
