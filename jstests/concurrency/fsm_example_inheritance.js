import {$config as $baseConfig} from "jstests/concurrency/fsm_example.js";
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";

// extendWorkload takes a $config object and a callback, and returns an extended $config object.
export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // In the callback, $super is the base workload definition we're
    // extending,
    // and $config is the extended workload definition we're creating.

    // You can replace any properties on $config, including methods you
    // want to override.
    $config.setup = function (db, collName, cluster) {
        // Overridden methods should usually call the corresponding
        // method on $super.
        $super.setup.apply(this, arguments);

        db[collName].createIndex({exampleIndexedField: 1});
    };

    return $config;
});
