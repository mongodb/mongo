/**
 * Loading this file overrides DBCollection.prototype.getIndexes() and aliases 'getIndices' and
 * 'getIndexSpecs' with an implementation that hides column store indexes from the output. This is
 * intended to increase the number of tests that can run when a column store index is implicitly
 * added to every collection.
 */
(function() {
'use strict';

load("jstests/libs/override_methods/override_helpers.js");  // For 'OverrideHelpers'.

DBCollection.prototype.getIndexes = function(filter) {
    return this
        .aggregate([
            {$indexStats: {}},
            {$match: filter || {}},
            // Hide the implicitly created index from tests that look for indexes
            {$match: {name: {$ne: "$**_columnstore"}}},
            // The information listed in 'spec' is usually returned inline at the root level.
            {$replaceWith: {$mergeObjects: ["$$ROOT", "$spec"]}},
            // This info isn't shown in 'getIndexes'.
            {$project: {host: 0, accesses: 0, spec: 0}},
        ])
        .toArray();
};

DBCollection.prototype.getIndices = DBCollection.prototype.getIndexes;
DBCollection.prototype.getIndexSpecs = DBCollection.prototype.getIndexes;

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/hide_column_store_indexes_from_get_indexes.js");
}());
