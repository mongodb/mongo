'use strict';

/**
 * compact_simultaneous_padding_bytes.js
 *
 * Bulk inserts 1000 documents and builds indexes. Then alternates between compacting the
 * collection and verifying the number of documents and indexes. Operates on a single collection
 * for all threads. Uses paddingBytes as a parameter for compact.
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/compact.js');              // for $config
load('jstests/concurrency/fsm_workload_helpers/server_types.js');  // for isEphemeral

var $config = extendWorkload($config, function($config, $super) {
    $config.states.init = function init(db, collName) {
        this.threadCollName = collName;
    };

    $config.states.compact = function compact(db, collName) {
        var res =
            db.runCommand({compact: this.threadCollName, paddingBytes: 1024 * 5, force: true});
        if (!isEphemeral(db)) {
            assertAlways.commandWorked(res);
        } else {
            assertAlways.commandFailedWithCode(res, ErrorCodes.CommandNotSupported);
        }
    };

    // no-op the query state because querying while compacting can result in closed cursors
    // as per SERVER-3964, as well as inaccurate counts, leaving nothing to assert.
    $config.states.query = function query(db, collName) {};

    return $config;
});
