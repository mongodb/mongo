'use strict';

/**
 * Executes the create_index_background.js workload, but with a partial filter expression on the
 * indexed field
 *
 * @tags: [creates_background_indexes]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');               // For extendWorkload.
load('jstests/concurrency/fsm_workloads/create_index_background.js');  // For $config.

var $config = extendWorkload($config, function($config, $super) {
    const fieldName = "isIndexed";

    $config.data.getIndexSpec = function() {
        return {[fieldName]: 1};
    };

    $config.data.getPartialFilterExpression = function() {
        return {[fieldName]: 1};
    };

    $config.data.extendUpdateExpr = function extendUpdateExpr(updateExpr) {
        // Set the field so that it may change whether or not it still applies to the partial index.
        updateExpr['$set'] = {[fieldName]: Random.randInt(2)};
        return updateExpr;
    };

    $config.data.extendDocument = function extendDocument(originalDoc) {

        // Be sure we're not overwriting an existing field.
        assertAlways.eq(originalDoc.hasOwnProperty(fieldName), false);

        // Create documents so that about half are indexed by applying to the partial filter
        // expression.
        originalDoc[fieldName] = Random.randInt(2);
        return originalDoc;
    };

    $config.setup = function setup() {
        $super.setup.apply(this, arguments);
    };

    return $config;
});
