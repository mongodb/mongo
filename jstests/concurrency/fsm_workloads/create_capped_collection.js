'use strict';

/**
 * create_capped_collection.js
 *
 * Repeatedly creates a capped collection. Also verifies that truncation
 * occurs once the collection reaches a certain size.
 */
load('jstests/concurrency/fsm_workload_helpers/drop_utils.js'); // for dropCollections

var $config = (function() {

    // Returns a document of the form { _id: ObjectId(...), field: '...' }
    // with specified BSON size.
    function makeDocWithSize(targetSize) {
        var doc = { _id: new ObjectId(), field: '' };

        var size = Object.bsonsize(doc);
        assertAlways.gte(targetSize, size);

        // Set 'field' as a string with enough characters
        // to make the whole document 'size' bytes long
        doc.field = new Array(targetSize - size + 1).join('x');
        assertAlways.eq(targetSize, Object.bsonsize(doc));

        return doc;
    }

    // Inserts a document of a certain size into the specified collection
    // and returns its _id field.
    function insert(db, collName, targetSize) {
        var doc = makeDocWithSize(targetSize);

        var res = db[collName].insert(doc);
        assertAlways.writeOK(res);
        assertAlways.eq(1, res.nInserted);

        return doc._id;
    }

    // Returns an array containing the _id fields of all the documents
    // in the collection, sorted according to their insertion order.
    function getObjectIds(db, collName) {
        return db[collName].find({}, { _id: 1 }).map(function(doc) {
            return doc._id;
        });
    }

    var data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'create_capped_collection',
        insert: insert,
        getObjectIds: getObjectIds
    };

    var states = (function() {

        var options = {
            capped: true,
            size: 8192 // multiple of 256; larger than 4096 default
        };

        function uniqueCollectionName(prefix, tid, num) {
            return prefix + tid + '_' + num;
        }

        function init(db, collName) {
            this.num = 0;
        }

        // TODO: how to avoid having too many files open?
        function create(db, collName) {
            var myCollName = uniqueCollectionName(this.prefix, this.tid, this.num++);
            assertAlways.commandWorked(db.createCollection(myCollName, options));

            // Define a large document to be half the size of the capped collection,
            // and a small document to be an eighth the size of the capped collection.
            var largeDocSize = Math.floor(options.size / 2) - 1;
            var smallDocSize = Math.floor(options.size / 8) - 1;

            var ids = [];
            var count;

            ids.push(this.insert(db, myCollName, largeDocSize));
            ids.push(this.insert(db, myCollName, largeDocSize));

            assertWhenOwnDB.contains(db[myCollName].find().itcount(), [1, 2]);

            // Insert another large document and verify that at least one
            // truncation has occurred. There may be 1 or 2 documents in
            // the collection, depending on the storage engine, but they
            // should always be the most recently inserted documents.

            ids.push(this.insert(db, myCollName, largeDocSize));

            count = db[myCollName].find().itcount();
            assertWhenOwnDB.contains(count, [1, 2], 'expected truncation to occur');
            assertWhenOwnDB.eq(ids.slice(ids.length - count), this.getObjectIds(db, myCollName));

            // Insert multiple small documents and verify that at least one
            // truncation has occurred. There may be 4 or 5 documents in
            // the collection, depending on the storage engine, but they
            // should always be the most recently inserted documents.

            ids.push(this.insert(db, myCollName, smallDocSize));
            ids.push(this.insert(db, myCollName, smallDocSize));
            ids.push(this.insert(db, myCollName, smallDocSize));
            ids.push(this.insert(db, myCollName, smallDocSize));

            var prevCount = count;
            count = db[myCollName].find().itcount();

            if (prevCount === 1) {
                assertWhenOwnDB.eq(4, count, 'expected truncation to occur');
            } else { // prevCount === 2
                assertWhenOwnDB.eq(5, count, 'expected truncation to occur');
            }

            assertWhenOwnDB.eq(ids.slice(ids.length - count), this.getObjectIds(db, myCollName));
        }

        return {
            init: init,
            create: create
        };

    })();

    var transitions = {
        init: { create: 1 },
        create: { create: 1 }
    };

    function teardown(db, collName) {
        var pattern = new RegExp('^' + this.prefix + '\\d+_\\d+$');
        dropCollections(db, pattern);
    }

    return {
        threadCount: 5,
        iterations: 20,
        data: data,
        states: states,
        transitions: transitions,
        teardown: teardown
    };

})();
