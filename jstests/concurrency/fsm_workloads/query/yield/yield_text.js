/*
 * yield_text.js (extends yield.js)
 *
 * Intersperse queries which use the TEXT stage with updates and deletes of documents they may
 * match.
 * @tags: [
 *   requires_getmore,
 *   # This test relies on query commands returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/yield/yield.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    /*
     * Pick a random word and search for it using full text search.
     */
    $config.states.query = function text(db, collName) {
        let word = this.words[Random.randInt(this.words.length)];

        let cursor = db[collName].find({$text: {$search: word}, yield_text: {$exists: true}}).batchSize(this.batchSize);

        let verifier = function textVerifier(doc, prevDoc) {
            return doc.yield_text.indexOf(word) !== -1;
        };
        this.advanceCursor(cursor, verifier);
    };

    $config.data.genUpdateDoc = function genUpdateDoc() {
        let newWord = this.words[Random.randInt(this.words.length)];
        return {$set: {yield_text: newWord}};
    };

    $config.setup = function setup(db, collName, cluster) {
        $super.setup.apply(this, arguments);

        assert.commandWorked(db[collName].createIndex({yield_text: "text"}));
    };

    return $config;
});
