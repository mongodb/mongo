'use strict';

/*
 * yield_rooted_or.js (extends yield.js)
 *
 * Intersperse queries which use a rooted OR stage with updates and deletes of documents they may
 * match.
 * Other workloads that need an index on c and d can inherit from this.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield.js');       // for $config

var $config = extendWorkload($config, function($config, $super) {

    /*
     * Issue a query with an or stage as the root.
     */
    $config.states.query = function rootedOr(db, collName) {
        var nMatches = 100;

        var cursor = db[collName]
                         .find({$or: [{c: {$lte: nMatches / 2}}, {d: {$lte: nMatches / 2}}]})
                         .batchSize(this.batchSize);

        var verifier = function rootedOrVerifier(doc, prevDoc) {
            return (doc.c <= nMatches / 2 || doc.d <= nMatches / 2);
        };

        this.advanceCursor(cursor, verifier);
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        var newC = Random.randInt(this.nDocs);
        var newD = Random.randInt(this.nDocs);
        return {$set: {c: newC, d: newD}};
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        assertAlways.commandWorked(db[collName].ensureIndex({c: 1}));
        assertAlways.commandWorked(db[collName].ensureIndex({d: 1}));
    };

    return $config;
});
