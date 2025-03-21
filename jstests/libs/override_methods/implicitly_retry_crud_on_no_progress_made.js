/**
 * Overrides some CRUD methods to retry on the 'NoProgressMade' error for use in concurrency suites
 * that run with the balancer. CRUD operations that rely on bulk execution can be blocked by
 * long-running re-sharding operations and fail with the 'NoProgressMade' error.
 * This is fine in production but can cause unnecessary noise in tests.
 */

jsTestLog("Running with retry on 'NoProgressMade' error override for CRUD operations");

function retryOnNoProgressMade(originalFunc, context, args) {
    let result = "";

    assert.soon(() => {
        try {
            result = originalFunc.apply(context, args);
            return true;
        } catch (e) {
            if (e instanceof BulkWriteError && e.hasWriteErrors()) {
                for (let writeErr of e.getWriteErrors()) {
                    if (writeErr.code == ErrorCodes.NoProgressMade) {
                        print(`No progress made while inserting documents. Received error ${
                            tojson(writeErr.code)}, Retrying.`);
                        return false;
                    }
                }
            } else {
                throw e;
            }
        }
    });
    return result;
}

// Save a reference to the original methods in the IIFE's scope.
// This scoping allows the original methods to be called by the overrides below.
const originalInsert = DBCollection.prototype.insert;
const originalInsertOne = DBCollection.prototype.insertOne;
const originalInsertMany = DBCollection.prototype.insertMany;
const originalUpdate = DBCollection.prototype.update;
const originalUpdateOne = DBCollection.prototype.updateOne;
const originalUpdateMany = DBCollection.prototype.updateMany;
const originalRemove = DBCollection.prototype.remove;
const originalDeleteOne = DBCollection.prototype.deleteOne;
const originalDeleteMany = DBCollection.prototype.deleteMany;
const originalReplaceOne = DBCollection.prototype.replaceOne;

DBCollection.prototype.insert = function(obj, options) {
    return retryOnNoProgressMade(originalInsert, this, [obj, options]);
};

DBCollection.prototype.insertOne = function(document, options) {
    return retryOnNoProgressMade(originalInsertOne, this, [document, options]);
};

DBCollection.prototype.insertMany = function(documents, options) {
    return retryOnNoProgressMade(originalInsertMany, this, [documents, options]);
};

DBCollection.prototype.update = function(query, updateSpec, upsert, multi) {
    return retryOnNoProgressMade(originalUpdate, this, [query, updateSpec, upsert, multi]);
};

DBCollection.prototype.updateOne = function(filter, update, options) {
    return retryOnNoProgressMade(originalUpdateOne, this, [filter, update, options]);
};

DBCollection.prototype.updateMany = function(filter, update, options) {
    return retryOnNoProgressMade(originalUpdateMany, this, [filter, update, options]);
};

DBCollection.prototype.remove = function(t, justOne) {
    return retryOnNoProgressMade(originalRemove, this, [t, justOne]);
};

DBCollection.prototype.deleteOne = function(filter, options) {
    return retryOnNoProgressMade(originalDeleteOne, this, [filter, options]);
};

DBCollection.prototype.deleteMany = function(filter, options) {
    return retryOnNoProgressMade(originalDeleteMany, this, [filter, options]);
};

DBCollection.prototype.replaceOne = function(filter, replacement, options) {
    return retryOnNoProgressMade(originalReplaceOne, this, [filter, replacement, options]);
};
