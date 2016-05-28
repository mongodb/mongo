'use strict';

/*
 * yield_fetch.js (extends yield_rooted_or.js)
 *
 * Intersperse queries which use the FETCH stage with updates and deletes of documents they may
 * match.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');       // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield_rooted_or.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    /*
     * Issue a query that will use the FETCH stage.
     */
    $config.states.query = function fetch(db, collName) {
        var nMatches = 100;

        var cursor = db[collName].find({c: {$lt: nMatches}}).batchSize(this.batchSize);

        var verifier = function fetchVerifier(doc, prevDoc) {
            return doc.c < nMatches;
        };

        this.advanceCursor(cursor, verifier);
    };

    return $config;
});
