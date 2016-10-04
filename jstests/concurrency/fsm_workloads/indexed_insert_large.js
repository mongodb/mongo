'use strict';

/**
 * indexed_insert_large.js
 *
 * Inserts multiple documents into an indexed collection. Asserts that all
 * documents appear in both a collection scan and an index scan. The indexed
 * value is a string large enough to make the whole index key be 1K, which is
 * the maximum.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');           // for extendWorkload
load('jstests/concurrency/fsm_workloads/indexed_insert_base.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    $config.data.indexedField = 'indexed_insert_large';

    // Remove the shard key, since it cannot be greater than 512 bytes
    delete $config.data.shardKey;

    $config.states.init = function init(db, collName) {
        $super.states.init.apply(this, arguments);

        // "The total size of an index entry, which can include structural overhead depending on
        // the
        //  BSON type, must be less than 1024 bytes."
        // http://docs.mongodb.org/manual/reference/limits/
        var maxIndexedSize = 1023;

        var bsonOverhead = Object.bsonsize({'': ''});

        var bigstr = new Array(maxIndexedSize + 1).join('x');

        // prefix the big string with tid to make it unique,
        // then trim it down so that it plus bson overhead is maxIndexedSize

        this.indexedValue = (this.tid + bigstr).slice(0, maxIndexedSize - bsonOverhead);

        assertAlways.eq(maxIndexedSize,
                        Object.bsonsize({'': this.indexedValue}),
                        'buggy test: the inserted docs will not have the expected index-key size');
    };

    return $config;
});
