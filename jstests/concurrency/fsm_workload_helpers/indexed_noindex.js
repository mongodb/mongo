/**
 * indexed_noindex.js
 *
 * Defines a modifier for indexed workloads that drops the index, specified by
 * $config.data.getIndexSpec(), at the end of the workload setup.
 */
import {assertAlways} from "jstests/concurrency/fsm_libs/assert.js";

export function indexedNoindex($config, $super) {
    $config.setup = function(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        var res = db[collName].dropIndex(this.getIndexSpec());
        assertAlways.commandWorked(res);
    };

    return $config;
}
