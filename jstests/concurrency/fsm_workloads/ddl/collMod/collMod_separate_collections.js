/**
 * collMod_separate_collections.js
 *
 * Generates some random data and inserts it into a collection with a
 * TTL index. Runs a collMod command to change the value of the
 * expireAfterSeconds setting to a random integer.
 *
 * Each thread updates a TTL index on a separate collection.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/ddl/collMod/collMod.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    $config.data.prefix = 'collMod_separate_collections';
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

    $config.startState = 'init';
    return $config;
});
