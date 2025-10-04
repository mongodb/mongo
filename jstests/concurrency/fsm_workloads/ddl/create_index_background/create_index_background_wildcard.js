/**
 * Executes the create_index_background.js workload, but with a wildcard index.
 *
 * This workload implicitly assumes that its tid ranges are [0, $config.threadCount). This
 * isn't guaranteed to be true when they are run in parallel with other workloads. Therefore
 * it can't be run in concurrency simultaneous suites.
 * @tags: [
 *   assumes_balancer_off,
 *   creates_background_indexes,
 *   requires_getmore,
 *   incompatible_with_concurrency_simultaneous,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/ddl/create_index_background/create_index_background.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.getIndexSpec = function () {
        return {"$**": 1};
    };

    $config.data.extendDocument = function extendDocument(originalDoc) {
        const fieldName = "arrayField";

        // Be sure we're not overwriting an existing field.
        assert.eq(originalDoc.hasOwnProperty(fieldName), false);

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
