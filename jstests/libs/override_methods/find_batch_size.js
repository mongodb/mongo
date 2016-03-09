/**
 * Loading this file overrides DBCollection.prototype.find() with a function that sets the default
 * value for batchSize to the value specified by TestData.batchSize.
 * Note - If batchSize is specified in either db.coll.find() or cursor.batchSize(),
 *        then that value is applied instead.
 */

// TODO: Add support for overriding batch sizes in DBQuery.prototype.clone.
// TODO: Add support for overriding batch sizes in DBCommandCursor.prototype._runGetMoreCommand.
// TODO: Add support for overriding batch sizes in the bulk API.

(function() {
    'use strict';

    // Save a reference to the original find method in the IIFE's scope.
    // This scoping allows the original method to be called by the find override below.
    var originalFind = DBCollection.prototype.find;

    DBCollection.prototype.find = function(query, fields, limit, skip, batchSize, options) {
        var batchSizeDefault = batchSize || (TestData && TestData.batchSize);
        return originalFind.call(this, query, fields, limit, skip, batchSizeDefault, options);
    };
}());
