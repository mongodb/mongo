/**
 * create_capped_collection_maxdocs.js
 *
 * Repeatedly creates a capped collection. Also verifies that truncation
 * occurs once the collection reaches a certain size or contains a
 * certain number of documents.
 *
 * As of SERVER-16049, capped deletes are replicated. This means that capped deletes can be rolled
 * back without rolling back the insert that caused the capped collection to trigger the delete.
 * This makes it possible for the capped collection to exceed its capped limit temporarily until
 * the next insert is performed successfully without rolling back.
 *
 * @tags: [does_not_support_stepdowns, requires_capped, requires_getmore]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/ddl/create_collection/create_capped_collection.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    // Use the workload name as a prefix for the collection name,
    // since the workload name is assumed to be unique.
    $config.data.prefix = "create_capped_collection_maxdocs";

    let options = {
        capped: true,
        size: 8192, // multiple of 256; larger than 4096 default
        max: 3,
    };

    function uniqueCollectionName(prefix, tid, num) {
        return prefix + tid + "_" + num;
    }

    // TODO: how to avoid having too many files open?
    function create(db, collName) {
        let myCollName = uniqueCollectionName(this.prefix, this.tid, this.num++);
        assert.commandWorked(db.createCollection(myCollName, options));

        // Define a small document to be an eighth the size of the capped collection.
        let smallDocSize = Math.floor(options.size / 8) - 1;

        // Verify size functionality still works as we expect
        this.verifySizeTruncation(db, myCollName, options);

        // Insert multiple small documents and verify that at least one truncation has occurred.
        // There should never be more than 3 documents in the collection, regardless of the
        // storage engine. They should always be the most recently inserted documents.

        let insertedIds = [];

        insertedIds.push(this.insert(db, myCollName, smallDocSize));
        insertedIds.push(this.insert(db, myCollName, smallDocSize));

        for (let i = 0; i < 50; i++) {
            insertedIds.push(this.insert(db, myCollName, smallDocSize));

            let foundIds = this.getObjectIds(db, myCollName);
            let count = foundIds.length;
            assert.eq(3, count, "expected truncation to occur due to number of docs");
            assert.eq(
                insertedIds.slice(insertedIds.length - count),
                foundIds,
                "expected truncation to remove the oldest documents",
            );
        }
    }

    $config.states.create = create;

    return $config;
});
