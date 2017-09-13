'use strict';

/**
 * drop_all_indexes.js
 *
 * Defines a modifier for workloads that drops all indexes created by the
 * base workload's setup function. The implicit _id index and any indexes
 * that already existed at the start of setup are not dropped.
 */

function dropAllIndexes($config, $super) {
    $config.setup = function setup(db, collName, cluster) {
        var oldIndexes = db[collName].getIndexes().map(function(ixSpec) {
            return ixSpec.name;
        });

        $super.setup.apply(this, arguments);

        // drop each index that wasn't present before calling super
        db[collName].getIndexes().forEach(function(ixSpec) {
            var name = ixSpec.name;
            if (name !== '_id_' && !Array.contains(oldIndexes, name)) {
                var res = db[collName].dropIndex(name);
                assertAlways.commandWorked(res);
            }
        });
    };

    return $config;
}
