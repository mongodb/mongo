'use strict';

/**
 * Executes the create_index_background.js workload, but with a wildcard index.
 *
 * SERVER-36709: Disabled for ephemeralForTest due to excessive memory usage
 * @tags: [creates_background_indexes, incompatible_with_eft]
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');               // For extendWorkload.
load('jstests/concurrency/fsm_workloads/create_index_background.js');  // For $config.

var $config = extendWorkload($config, function($config, $super) {
    $config.data.getIndexSpec = function() {
        return {"$**": 1};
    };

    $config.data.extendDocument = function extendDocument(originalDoc) {
        const fieldName = "arrayField";

        // Be sure we're not overwriting an existing field.
        assertAlways.eq(originalDoc.hasOwnProperty(fieldName), false);

        // Insert a field which has an array as the value, to exercise the special multikey
        // metadata functionality wildcard indexes rely on.
        originalDoc[fieldName] = [1, 2, "string", this.tid];
        return originalDoc;
    };

    $config.setup = function setup() {
        $super.setup.apply(this, arguments);
    };

    return $config;
});
