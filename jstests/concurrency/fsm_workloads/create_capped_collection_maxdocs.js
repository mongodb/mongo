'use strict';

/**
 * create_capped_collection_maxdocs.js
 *
 * Repeatedly creates a capped collection. Also verifies that truncation
 * occurs once the collection reaches a certain size or contains a
 * certain number of documents.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');                // for extendWorkload
load('jstests/concurrency/fsm_workloads/create_capped_collection.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {

    // Use the workload name as a prefix for the collection name,
    // since the workload name is assumed to be unique.
    $config.data.prefix = 'create_capped_collection_maxdocs';

    var options = {
        capped: true,
        size: 8192,  // multiple of 256; larger than 4096 default
        max: 3
    };

    function uniqueCollectionName(prefix, tid, num) {
        return prefix + tid + '_' + num;
    }

    // TODO: how to avoid having too many files open?
    function create(db, collName) {
        var myCollName = uniqueCollectionName(this.prefix, this.tid, this.num++);
        assertAlways.commandWorked(db.createCollection(myCollName, options));

        // Define a small document to be an eighth the size of the capped collection.
        var smallDocSize = Math.floor(options.size / 8) - 1;

        // Verify size functionality still works as we expect
        this.verifySizeTruncation(db, myCollName, options);

        // Insert multiple small documents and verify that at least one truncation has occurred.
        // There should never be more than 3 documents in the collection, regardless of the
        // storage
        // engine. They should always be the most recently inserted documents.

        var ids = [];
        var count;

        ids.push(this.insert(db, myCollName, smallDocSize));
        ids.push(this.insert(db, myCollName, smallDocSize));

        for (var i = 0; i < 50; i++) {
            ids.push(this.insert(db, myCollName, smallDocSize));
            count = db[myCollName].find().itcount();
            assertWhenOwnDB.eq(3, count, 'expected truncation to occur due to number of docs');
            assertWhenOwnDB.eq(ids.slice(ids.length - count),
                               this.getObjectIds(db, myCollName),
                               'expected truncation to remove the oldest documents');
        }
    }

    $config.states.create = create;

    return $config;
});
