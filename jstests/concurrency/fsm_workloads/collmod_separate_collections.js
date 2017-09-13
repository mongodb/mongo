'use strict';

/**
 * collmod_separate_collections.js
 *
 * Generates some random data and inserts it into a collection with a
 * TTL index. Runs a collMod command to change the value of the
 * expireAfterSeconds setting to a random integer.
 *
 * Each thread updates a TTL index on a separate collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/collmod.js');            // for $config
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js');  // for dropCollections

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = 'collmod_separate_collections';
    $config.data.shardKey = {createdAt: 1};

    $config.states.init = function init(db, collName) {
        this.threadCollName = this.prefix + '_' + this.tid;
        $super.setup.call(this, db, this.threadCollName);
    };

    $config.transitions = Object.extend({init: {collMod: 1}}, $super.transitions);

    $config.setup = function setup(db, collName, cluster) {
        // no-op: since the init state is used to setup
        // the separate collections on a per-thread basis.
    };

    $config.teardown = function teardown(db, collName, cluster) {
        var pattern = new RegExp('^' + this.prefix + '_\\d+$');
        dropCollections(db, pattern);
        $super.teardown.apply(this, arguments);
    };

    $config.startState = 'init';
    return $config;
});
