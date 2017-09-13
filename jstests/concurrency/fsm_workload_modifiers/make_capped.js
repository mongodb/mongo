'use strict';

/**
 * make_capped.js
 *
 * Defines a modifier for workloads that drops the collection and re-creates it
 * as capped at the start of setup.
 *
 * However, it only does this when it owns the collection, to avoid surprising
 * other workloads.
 *
 * It also modifies any state named 'find' to run with a weaker assertion level:
 * only assertAlways assertions will run in that state.
 */

function makeCapped($config, $super) {
    $config.setup = function setup(db, collName, cluster) {
        assertWhenOwnColl(function() {
            db[collName].drop();
            assertAlways.commandWorked(db.createCollection(collName, {
                capped: true,
                size: 16384  // bytes
            }));
        });

        $super.setup.apply(this, arguments);
    };

    if ($super.states.find) {
        $config.states.find = function find(db, collName) {
            var oldAssertLevel = globalAssertLevel;
            try {
                // Temporarily weaken the global assertion level to avoid spurious
                // failures due to collection truncation
                globalAssertLevel = AssertLevel.ALWAYS;
                $super.states.find.apply(this, arguments);
            } catch (e) {
                if (e.message.indexOf('CappedPositionLost') >= 0) {
                    // Ignore errors when a cursor's position in the capped collection is deleted.
                    // Reads from the beginning of a capped collection are not guaranteed to succeed
                    // when there are concurrent inserts that cause a truncation.
                    return;
                }
                throw e;
            } finally {
                globalAssertLevel = oldAssertLevel;
            }
        };
    }

    return $config;
}
