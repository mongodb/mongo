'use strict';

/**
 * indexed_insert_wildcard.js
 *
 * Inserts documents into an indexed collection and asserts that the documents appear in both a
 * collection scan and an index scan. The collection is indexed with a wildcard index.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // For extendWorkload().
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js');  // For $config().

var $config = extendWorkload($config, function($config, $super) {
    $config.setup = function init(db, collName) {
        $super.setup.apply(this, arguments);
    };

    $config.data.getDoc = function getDoc() {
        // Return a document which has arrays, nested arrays, and sub documents.
        return {
            threadIdA: this.tid,
            threadIdB: this.tid,
            threadIdC: this.tid,
            threadIdInArray: [this.tid],
            nestedThreadId: {threadId: this.tid},
            arrayField: [this.tid, "a string", [1, 2, 3]],
            fieldWithNestedObject: {nestedDoc: {subNestedDoc: {leaf: "a string"}}, leaf: "a string"}
        };
    };

    $config.data.getQuery = function getQuery() {
        // Choose a field to query on (all have the same value, but just gives some variety in what
        // type of queries we run).
        const possibleFields =
            ["threadIdA", "threadIdB", "threadIdC", "threadIdInArray", "nestedThreadId.threadId"];
        const chosenField = possibleFields[Math.floor(Math.random() * possibleFields.length)];
        return {[chosenField]: this.tid};
    };

    $config.data.getIndexSpec = function getIndexSpec() {
        return {"$**": 1};
    };

    // Remove the shard key, since a wildcard index cannot be used to index the shard key.
    delete $config.data.shardKey;

    return $config;
});
