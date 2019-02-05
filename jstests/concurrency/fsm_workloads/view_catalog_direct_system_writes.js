'use strict';

/**
 * view_catalog_direct_system_writes.js
 *
 * Extends 'view_catalog.js' in concurrently creating, modifying and dropping view namespaces, but
 * does so via direct writes to system.views instead of using the collMod or drop commands. Each
 * worker operates on their own view, built on a shared underlying collection.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');    // for extendWorkload
load('jstests/concurrency/fsm_workloads/view_catalog.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.states.create = function create(db, collName) {
        this.counter++;
        let pipeline = [{$match: {_id: this.counter}}];
        assertAlways.commandWorked(db.system.views.insert({
            _id: db.getName() + "." + this.threadViewName,
            viewOn: this.threadCollName,
            pipeline: pipeline
        }));
        this.confirmViewDefinition(db, this.threadViewName, collName, pipeline, this.counter);
    };

    $config.states.drop = function drop(db, collName) {
        assertAlways.commandWorked(
            db.system.views.deleteOne({_id: db.getName() + "." + this.threadViewName}));

        let res = db.runCommand({listCollections: 1, filter: {name: this.threadViewName}});
        assertAlways.commandWorked(res);
        assertAlways.eq(0, res.cursor.firstBatch.length, tojson(res));
    };

    // Unfortunately we cannot perform an update in the place of a collMod since the update would
    // contain a $-prefixed field (the $match from the pipeline, and so would be rejected by the
    // update system. This is okay, the drop override below is enough to reproduce the issue seen in
    // SERVER-37283. Because of this, we modify the transitions to favor going to drop more often.
    $config.transitions = {
        init: {create: 1},
        create: {modify: 0.5, drop: 0.5},
        modify: {modify: 0.3, drop: 0.7},
        drop: {create: 1}
    };

    return $config;
});
