'use strict';

/*
 * yield_text.js (extends yield.js)
 *
 * Intersperse queries which use the TEXT stage with updates and deletes of documents they may
 * match.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');  // for extendWorkload
load('jstests/concurrency/fsm_workloads/yield.js');       // for $config

var $config = extendWorkload($config, function($config, $super) {

    /*
     * Pick a random word and search for it using full text search.
     */
    $config.states.query = function text(db, collName) {
        var word = this.words[Random.randInt(this.words.length)];

        var cursor = db[collName]
                         .find({$text: {$search: word}, yield_text: {$exists: true}})
                         .batchSize(this.batchSize);

        var verifier = function textVerifier(doc, prevDoc) {
            return doc.yield_text.indexOf(word) !== -1;
        };

        // If we don't have the right text index, or someone drops our text index, this
        // assertion
        // is either pointless or won't work. So only verify the results when we know no one
        // else
        // is messing with our indices.
        assertWhenOwnColl(function verifyTextResults() {
            this.advanceCursor(cursor, verifier);
        }.bind(this));
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        var newWord = this.words[Random.randInt(this.words.length)];
        return {$set: {yield_text: newWord}};
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        assertWhenOwnColl.commandWorked(db[collName].ensureIndex({yield_text: 'text'}));
    };

    return $config;
});
