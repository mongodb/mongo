DBCollection.prototype._createWriteConcern = function (options) {
    // If writeConcern set, use it, else get from collection (which will inherit from db/mongo)
    let writeConcern = options.writeConcern || this.getWriteConcern();
    let writeConcernOptions = ["w", "wtimeout", "j", "fsync"];

    if (writeConcern instanceof WriteConcern) {
        writeConcern = writeConcern.toJSON();
    }

    // Only merge in write concern options if at least one is specified in options
    if (options.w != null || options.wtimeout != null || options.j != null || options.fsync != null) {
        writeConcern = {};

        writeConcernOptions.forEach(function (wc) {
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
DBCollection.prototype.addIdIfNeeded = function (obj) {
    if (typeof obj !== "object") {
        throw new Error("argument passed to addIdIfNeeded is not an object");
    }
    if (typeof obj._id == "undefined" && !Array.isArray(obj)) {
        let tmp = obj; // don't want to modify input
        obj = {_id: new ObjectId()};

        for (let key in tmp) {
            if (tmp.hasOwnProperty(key)) {
                obj[key] = tmp[key];
            }
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
 *  { updateOne: { filter: {a:2}, update: {$set: {"a.$[i]":2}}, upsert:true, collation: {locale:
 * "fr"}, arrayFilters: [{i: 0}] } }
 *
 *  { updateMany: { filter: {a:2}, update: {$set: {"a.$[i]":2}}, upsert:true collation: {locale:
 * "fr"}, arrayFilters: [{i: 0}] } }
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
DBCollection.prototype.bulkWrite = function (operations, options) {
    let opts = Object.extend({}, options || {});
    opts.ordered = typeof opts.ordered == "boolean" ? opts.ordered : true;

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Result
    let result = {acknowledged: !(writeConcern && writeConcern.w == 0)};

    // Use bulk operation API already in the shell
    let bulkOp = opts.ordered ? this.initializeOrderedBulkOp() : this.initializeUnorderedBulkOp();

    if (opts.rawData) {
        bulkOp.setRawData();
    }

    // Contains all inserted _ids
    let insertedIds = {};

    // For each of the operations we need to add the op to the bulk
    operations.forEach(function (op, index) {
        if (op.insertOne) {
            if (!op.insertOne.document) {
                throw new Error("insertOne bulkWrite operation expects the document field");
            }

            // Add _id ObjectId if needed
            op.insertOne.document = this.addIdIfNeeded(op.insertOne.document);
            // InsertedIds is a map of [originalInsertOrderIndex] = document._id
            insertedIds[index] = op.insertOne.document._id;
            // Translate operation to bulk operation
            bulkOp.insert(op.insertOne.document);
        } else if (op.updateOne) {
            if (!op.updateOne.filter) {
                throw new Error("updateOne bulkWrite operation expects the filter field");
            }

            if (!op.updateOne.update) {
                throw new Error("updateOne bulkWrite operation expects the update field");
            }

            // Translate operation to bulk operation
            let operation = bulkOp.find(op.updateOne.filter);
            if (op.updateOne.sort) {
                operation.sort(op.updateOne.sort);
            }

            if (op.updateOne.upsert) {
                operation = operation.upsert();
            }

            if (op.updateOne.hint) {
                operation = operation.hint(op.updateOne.hint);
            }

            if (op.updateOne.collation) {
                operation.collation(op.updateOne.collation);
            }

            if (op.updateOne.arrayFilters) {
                operation.arrayFilters(op.updateOne.arrayFilters);
            }

            operation.updateOne(op.updateOne.update);
        } else if (op.updateMany) {
            if (!op.updateMany.filter) {
                throw new Error("updateMany bulkWrite operation expects the filter field");
            }

            if (!op.updateMany.update) {
                throw new Error("updateMany bulkWrite operation expects the update field");
            }

            if (op.updateMany.sort) {
                throw new Error(
                    "This sort will not do anything. Please call update without a sort or defer to calling updateOne with a sort.",
                );
            }

            // Translate operation to bulk operation
            let operation = bulkOp.find(op.updateMany.filter);
            if (op.updateMany.upsert) {
                operation = operation.upsert();
            }

            if (op.updateMany.hint) {
                operation = operation.hint(op.updateMany.hint);
            }

            if (op.updateMany.collation) {
                operation.collation(op.updateMany.collation);
            }

            if (op.updateMany.arrayFilters) {
                operation.arrayFilters(op.updateMany.arrayFilters);
            }

            operation.update(op.updateMany.update);
        } else if (op.replaceOne) {
            if (!op.replaceOne.filter) {
                throw new Error("replaceOne bulkWrite operation expects the filter field");
            }

            if (!op.replaceOne.replacement) {
                throw new Error("replaceOne bulkWrite operation expects the replacement field");
            }

            // Translate operation to bulkOp operation
            let operation = bulkOp.find(op.replaceOne.filter);
            if (op.replaceOne.upsert) {
                operation = operation.upsert();
            }

            if (op.replaceOne.collation) {
                operation.collation(op.replaceOne.collation);
            }

            if (op.replaceOne.hint) {
                operation.hint(op.replaceOne.hint);
            }

            operation.replaceOne(op.replaceOne.replacement);
        } else if (op.deleteOne) {
            if (!op.deleteOne.filter) {
                throw new Error("deleteOne bulkWrite operation expects the filter field");
            }

            // Translate operation to bulkOp operation.
            let deleteOp = bulkOp.find(op.deleteOne.filter);

            if (op.deleteOne.collation) {
                deleteOp.collation(op.deleteOne.collation);
            }

            deleteOp.removeOne();
        } else if (op.deleteMany) {
            if (!op.deleteMany.filter) {
                throw new Error("deleteMany bulkWrite operation expects the filter field");
            }

            // Translate operation to bulkOp operation.
            let deleteOp = bulkOp.find(op.deleteMany.filter);

            if (op.deleteMany.collation) {
                deleteOp.collation(op.deleteMany.collation);
            }

            deleteOp.remove();
        }
    }, this);

    // Execute bulkOp operation
    let response = bulkOp.execute(writeConcern);
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
    let upserts = response.getUpsertedIds();
    upserts.forEach(function (x) {
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
DBCollection.prototype.insertOne = function (document, options) {
    let opts = Object.extend({}, options || {});

    // Add _id ObjectId if needed
    document = this.addIdIfNeeded(document);

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Result
    let result = {acknowledged: !(writeConcern && writeConcern.w == 0)};

    // Use bulk operation API already in the shell
    let bulk = this.initializeOrderedBulkOp();

    if (opts.rawData) bulk.setRawData(opts.rawData);

    bulk.insert(document);

    try {
        // Execute insert
        bulk.execute(writeConcern);
    } catch (err) {
        if (err instanceof BulkWriteError) {
            if (err.hasWriteErrors()) {
                throw err.getWriteErrorAt(0);
            }

            if (err.hasWriteConcernError()) {
                throw err.getWriteConcernError();
            }
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
DBCollection.prototype.insertMany = function (documents, options) {
    let opts = Object.extend({}, options || {});
    opts.ordered = typeof opts.ordered == "boolean" ? opts.ordered : true;

    // Ensure all documents have an _id
    documents = documents.map(function (x) {
        return this.addIdIfNeeded(x);
    }, this);

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Result
    let result = {acknowledged: !(writeConcern && writeConcern.w == 0)};

    // Use bulk operation API already in the shell
    let bulk = opts.ordered ? this.initializeOrderedBulkOp() : this.initializeUnorderedBulkOp();

    if (opts.rawData) bulk.setRawData(opts.rawData);

    // Add all operations to the bulk operation
    documents.forEach(function (doc) {
        bulk.insert(doc);
    });

    // Execute bulk write operation
    bulk.execute(writeConcern);

    if (!result.acknowledged) {
        return result;
    }

    // Set all the created inserts
    result.insertedIds = documents.map(function (x) {
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
DBCollection.prototype.deleteOne = function (filter, options) {
    let opts = Object.extend({}, options || {});

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Result
    let result = {acknowledged: !(writeConcern && writeConcern.w == 0)};

    // Use bulk operation API already in the shell
    let bulk = this.initializeOrderedBulkOp();
    let removeOp = bulk.find(filter);

    // Add the collation, if there is one.
    if (opts.collation) {
        removeOp.collation(opts.collation);
    }

    if (opts.rawData) {
        bulk.setRawData(opts.rawData);
    }

    // Add the deleteOne operation.
    removeOp.removeOne();

    let r;
    try {
        // Remove the first document that matches the selector
        r = bulk.execute(writeConcern);
    } catch (err) {
        if (err instanceof BulkWriteError) {
            if (err.hasWriteErrors()) {
                throw err.getWriteErrorAt(0);
            }

            if (err.hasWriteConcernError()) {
                throw err.getWriteConcernError();
            }
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
DBCollection.prototype.deleteMany = function (filter, options) {
    let opts = Object.extend({}, options || {});

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Result
    let result = {acknowledged: !(writeConcern && writeConcern.w == 0)};

    // Use bulk operation API already in the shell
    let bulk = this.initializeOrderedBulkOp();
    let removeOp = bulk.find(filter);

    // Add the collation, if there is one.
    if (opts.collation) {
        removeOp.collation(opts.collation);
    }

    if (opts.rawData) {
        bulk.setRawData(opts.rawData);
    }

    // Add the deleteOne operation.
    removeOp.remove();

    let r;
    try {
        // Remove all documents that matche the selector
        r = bulk.execute(writeConcern);
    } catch (err) {
        if (err instanceof BulkWriteError) {
            if (err.hasWriteErrors()) {
                throw err.getWriteErrorAt(0);
            }

            if (err.hasWriteConcernError()) {
                throw err.getWriteConcernError();
            }
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
DBCollection.prototype.replaceOne = function (filter, replacement, options) {
    let opts = Object.extend({}, options || {});

    // Cannot use pipeline-style updates in a replacement operation.
    if (Array.isArray(replacement)) {
        throw new Error("Cannot use pipeline-style updates in a replacement operation");
    }

    // Check if first key in update statement contains a $
    let keys = Object.keys(replacement);
    // Check if first key does not have the $
    if (keys.length > 0 && keys[0][0] == "$") {
        throw new Error("the replace operation document must not contain atomic operators");
    }

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Result
    let result = {acknowledged: !(writeConcern && writeConcern.w == 0)};

    // Use bulk operation API already in the shell
    let bulk = this.initializeOrderedBulkOp();

    // Add the deleteOne operation
    let op = bulk.find(filter);
    if (opts.upsert) {
        op = op.upsert();
    }

    if (opts.collation) {
        op.collation(opts.collation);
    }

    if (opts.hint) {
        op.hint(opts.hint);
    }

    if (opts.rawData) {
        bulk.setRawData(opts.rawData);
    }

    op.replaceOne(replacement);

    let r;
    try {
        // Replace the document
        r = bulk.execute(writeConcern);
    } catch (err) {
        if (err instanceof BulkWriteError) {
            if (err.hasWriteErrors()) {
                throw err.getWriteErrorAt(0);
            }

            if (err.hasWriteConcernError()) {
                throw err.getWriteConcernError();
            }
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    result.matchedCount = r.nMatched;
    result.modifiedCount = r.nModified != null ? r.nModified : r.n;

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
DBCollection.prototype.updateOne = function (filter, update, options) {
    let opts = Object.extend({}, options || {});

    // Pipeline updates are always permitted. Otherwise, we validate the update object.
    if (!Array.isArray(update)) {
        // Check if first key in update statement contains a $
        let keys = Object.keys(update);
        if (keys.length == 0) {
            throw new Error("the update operation document must contain at least one atomic operator");
        }
        // Check if first key does not have the $
        if (keys[0][0] != "$") {
            throw new Error("the update operation document must contain atomic operators");
        }
    }

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Result
    let result = {acknowledged: !(writeConcern && writeConcern.w == 0)};

    // Use bulk operation API already in the shell
    let bulk = this.initializeOrderedBulkOp();

    // Add the updateOne operation
    let op = bulk.find(filter);
    if (opts.sort) {
        op.sort(opts.sort);
    }
    if (opts.upsert) {
        op = op.upsert();
    }

    if (opts.hint) {
        op.hint(opts.hint);
    }

    if (opts.collation) {
        op.collation(opts.collation);
    }

    if (opts.arrayFilters) {
        op.arrayFilters(opts.arrayFilters);
    }

    if (opts.rawData) {
        bulk.setRawData(opts.rawData);
    }

    op.updateOne(update);

    let r;
    try {
        // Update the first document that matches the selector
        r = bulk.execute(writeConcern);
    } catch (err) {
        if (err instanceof BulkWriteError) {
            if (err.hasWriteErrors()) {
                throw err.getWriteErrorAt(0);
            }

            if (err.hasWriteConcernError()) {
                throw err.getWriteConcernError();
            }
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    result.matchedCount = r.nMatched;
    result.modifiedCount = r.nModified != null ? r.nModified : r.n;

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
DBCollection.prototype.updateMany = function (filter, update, options) {
    let opts = Object.extend({}, options || {});

    // Pipeline updates are always permitted. Otherwise, we validate the update object.
    if (!Array.isArray(update)) {
        // Check if first key in update statement contains a $
        let keys = Object.keys(update);
        if (keys.length == 0) {
            throw new Error("the update operation document must contain at least one atomic operator");
        }
        // Check if first key does not have the $
        if (keys[0][0] != "$") {
            throw new Error("the update operation document must contain atomic operators");
        }
    }

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Result
    let result = {acknowledged: !(writeConcern && writeConcern.w == 0)};

    // Use bulk operation API already in the shell
    let bulk = this.initializeOrderedBulkOp();

    // Add the updateMany operation
    let op = bulk.find(filter);

    if (opts.sort) {
        throw new Error(
            "This sort will not do anything. Please call update without a sort or defer to calling updateOne with a sort.",
        );
    }

    if (opts.upsert) {
        op = op.upsert();
    }

    if (opts.hint) {
        op.hint(opts.hint);
    }

    if (opts.collation) {
        op.collation(opts.collation);
    }

    if (opts.arrayFilters) {
        op.arrayFilters(opts.arrayFilters);
    }

    if (opts.rawData) {
        bulk.setRawData(opts.rawData);
    }

    op.update(update);

    let r;
    try {
        // Update all documents that match the selector
        r = bulk.execute(writeConcern);
    } catch (err) {
        if (err instanceof BulkWriteError) {
            if (err.hasWriteErrors()) {
                throw err.getWriteErrorAt(0);
            }

            if (err.hasWriteConcernError()) {
                throw err.getWriteConcernError();
            }
        }

        throw err;
    }

    if (!result.acknowledged) {
        return result;
    }

    result.matchedCount = r.nMatched;
    result.modifiedCount = r.nModified != null ? r.nModified : r.n;

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
DBCollection.prototype.findOneAndDelete = function (filter, options) {
    let opts = Object.extend({}, options || {});
    // Set up the command
    let cmd = {query: filter || {}, remove: true};

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

    if (opts.rawData) {
        cmd.rawData = opts.rawData;
    }

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

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
 * @param {object|string} [options.hint=null] Force a particular index to be used for the query.
 * @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
 * @param {boolean} [options.upsert=false] Upsert the document if it does not exist.
 * @param {boolean} [options.returnNewDocument=false] When true, returns the updated document rather
 *than the original. The default is false.
 * @return {object}
 */
DBCollection.prototype.findOneAndReplace = function (filter, replacement, options) {
    let opts = Object.extend({}, options || {});

    // Cannot use pipeline-style updates in a replacement operation.
    if (Array.isArray(replacement)) {
        throw new Error("Cannot use pipeline-style updates in a replacement operation");
    }

    // Check if first key in update statement contains a $
    let keys = Object.keys(replacement);
    // Check if first key does not have the $
    if (keys.length > 0 && keys[0][0] == "$") {
        throw new Error("the replace operation document must not contain atomic operators");
    }

    // Set up the command
    let cmd = {query: filter || {}, update: replacement};
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

    if (opts.hint) {
        cmd.hint = opts.hint;
    }

    if (opts.rawData) {
        cmd.rawData = opts.rawData;
    }

    // Set flags
    cmd.upsert = typeof opts.upsert == "boolean" ? opts.upsert : false;
    cmd.new = typeof opts.returnNewDocument == "boolean" ? opts.returnNewDocument : false;

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

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
 * @param {object|string} [options.hint=null] Force a particular index to be used for the query.
 * @param {number} [options.maxTimeMS=null] The maximum amount of time to allow the query to run.
 * @param {boolean} [options.upsert=false] Upsert the document if it does not exist.
 * @param {boolean} [options.returnNewDocument=false] When true, returns the updated document rather
 *than the original. The default is false.
 * @return {object}
 */
DBCollection.prototype.findOneAndUpdate = function (filter, update, options) {
    let opts = Object.extend({}, options || {});

    // Pipeline updates are always permitted. Otherwise, we validate the update object.
    if (!Array.isArray(update)) {
        // Check if first key in update statement contains a $
        let keys = Object.keys(update);
        if (keys.length == 0) {
            throw new Error("the update operation document must contain at least one atomic operator");
        }
        // Check if first key does not have the $
        if (keys[0][0] != "$") {
            throw new Error("the update operation document must contain atomic operators");
        }
    }

    // Set up the command
    let cmd = {query: filter || {}, update};
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

    if (opts.arrayFilters) {
        cmd.arrayFilters = opts.arrayFilters;
    }

    if (opts.hint) {
        cmd.hint = opts.hint;
    }

    if (opts.rawData) {
        cmd.rawData = opts.rawData;
    }

    // Set flags
    cmd.upsert = typeof opts.upsert == "boolean" ? opts.upsert : false;
    cmd.new = typeof opts.returnNewDocument == "boolean" ? opts.returnNewDocument : false;

    // Get the write concern
    let writeConcern = this._createWriteConcern(opts);

    // Setup the write concern
    if (writeConcern) {
        cmd.writeConcern = writeConcern;
    }

    // Execute findAndModify
    return this.findAndModify(cmd);
};
